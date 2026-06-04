# L18 Design — HTTP REST API for the data store

> ACE-backed HTTP/1.1 server with in-house parser, long-poll support,
> and optional native TLS / mutual-TLS termination. Mirror of the xpmile
> UniService pattern applied to the iot data store.

## 1. Architecture

```
                          ┌──────────────────────────────────────┐
                          │           iot-httpd                   │
                          │                                      │
  HTTP(S) client ───────▶ │  accept loop (poll ACE_SOCK_Acceptor)│
  (curl, browser,         │    │ new HttpSession(router, tls?)   │
   script)                │    ▼                                 │
                          │  HttpSession (ACE_Svc_Handler)       │
                          │    │ register with ACE_Reactor        │
                          │    │ handle_input():                  │
                          │    │   [TlsConn: decrypt if https]    │
                          │    ▼                                 │
                          │  HttpParser (in-house, push-based)    │
                          │    │ feed bytes → handler on complete │
                          │    ▼                                 │
                          │  Router: match (method, path)         │
                          │    ├── POST /api/v1/db/get           │
                          │    ├── POST /api/v1/db/set           │
                          │    └── GET  /api/v1/db/get?timeout=N │
                          │    │ handler runs INLINE (reactor     │
                          │    │ thread); closes over ds Client*  │
                          │    ▼                                 │
                          │  Response: HTTP/1.1 + JSON body      │
                          │    │ [TlsConn: encrypt if https]      │
                          │    ▼ send_bytes() → peer              │
                          │  data_store::Client* (one, shared)    │
                          │    │ unix socket → ds-server         │
                          └────┼──────────────────────────────────┘
                               │
                    ┌──────────▼──────────┐
                    │     ds-server        │
                    │  /var/run/iot/       │
                    │  data_store.sock     │
                    └─────────────────────┘
```

> **Note.** By default (`http.workers = 0`) handlers run **inline on the
> single reactor thread**, so a long-poll blocks the reactor. Set
> `http.workers = N` to run them on a thread pool (§2.8) — then only a
> worker blocks. See §2.5, §2.8 and D1.

## 2. Components

### 2.1 HttpParser (in-house)

Push-based, callback-driven HTTP/1.1 request parser. No external
dependency — hand-rolled following RFC 7230 minimal subset.

```
                    feed("GET /api/v1")
                         │
                    ┌────▼────┐
                    │  Method  │ → "GET"
                    │  URL     │ → "/api/v1/db/get?timeout=30"
                    │  Headers │ → map<string,string>
                    │  Body    │ → JSON (for POST)
                    └────┬────┘
                         │
                    on_request_complete()
```

**States:** `MethodLine → Headers → Body → Done`

**API:**
```cpp
class HttpParser {
public:
    /// Feed bytes. Returns the number consumed. When a complete
    /// request is parsed, `on_done` fires with the parse result.
    std::size_t feed(const char* data, std::size_t len);

    /// True when the parser has seen a complete request and is
    /// ready for the next one (keep-alive). False while parsing.
    bool done() const;

    /// Reset for the next request on the same connection.
    void reset();

    struct Request {
        std::string                          method;
        std::string                          url;
        std::string                          path;
        std::map<std::string, std::string>   query;
        std::map<std::string, std::string>   headers;
        std::string                          body;
    };

    /// Callback: parser invokes this when a complete request is
    /// assembled. The handler returns the response body.
    using Handler = std::function<std::string(const Request&)>;
    void set_handler(Handler h);

private:
    enum State { MethodLine, Headers, Body, Done };
    State     m_state = MethodLine;
    Request   m_req;
    Handler   m_handler;
    std::size_t m_content_length = 0;
    std::size_t m_body_read      = 0;
    std::string m_buf;  // rolling buffer for current parse
};
```

**Conformance:** HTTP/1.1 only. Persistent connections (keep-alive)
via `Connection: keep-alive`. Closes after each request by default.
Request bodies may be either `Content-Length`-delimited or
`Transfer-Encoding: chunked`; chunked wins when both are present
(RFC 7230 §3.3.3).

**Chunked decoding.** `parse_body()` dispatches to a resumable
sub-state machine (`Chunk::{Size, Data, DataCrlf, Trailer}`) that
dechunks into `m_req.body` as bytes arrive — so a body split across many
`feed()` calls works byte-for-byte. Chunk extensions (`size;ext`) and
trailer headers are accepted and ignored. A non-hex size errors out; the
decoded body is capped at 8 MiB (`kMaxBody`, also applied to the
`Content-Length` path) so an endless chunk stream can't exhaust memory.

### 2.2 HttpSession (ACE_Svc_Handler)

One per TCP connection. Registered with `ACE_Reactor` for
`READ_MASK`. The session owns the parser and (on https) a `TlsConn`; it
does **not** hold a `data_store::Client` — handlers close over that (§2.3).
On `handle_input()`:

1. `recv()` up to 4096 bytes into a buffer.
2. On https, feed the ciphertext to `TlsConn` (advance handshake, then
   decrypt); on plain HTTP the bytes are the request as-is.
3. `HttpParser::feed()` the (plaintext) bytes; the parser fires the
   handler set via `set_handler()` and captures its response string.
4. When `parser.done()`:
   - `send_bytes(parser.take_response())` — straight `send_n()` on plain
     HTTP, or `SSL_write` + drain on https.
   - If `Connection: keep-alive`: `parser.reset()`, stay registered.
   - Else: return `-1` (close).
   On `parser.error()`: send `400 Bad Request` and close.

```cpp
class HttpSession : public ACE_Svc_Handler<ACE_SOCK_Stream, ACE_MT_SYNCH> {
public:
    // tls == nullptr → plain HTTP; non-null → terminate TLS (§2.7).
    explicit HttpSession(const Router& router, const TlsContext* tls = nullptr);

    int handle_input(ACE_HANDLE fd) override;
    int handle_close(ACE_HANDLE fd, ACE_Reactor_Mask mask) override;

private:
    const Router&            m_router;
    HttpParser               m_parser;
    std::string              m_recv_buf;
    std::unique_ptr<TlsConn> m_tls;   // null on a plain-HTTP listener

    void send_bytes(const char* data, std::size_t len);  // clear or TLS
};
```

### 2.3 Router

Minimal path dispatcher. Matches `(method, path)` to handler
functions. The handler receives only the parsed request and returns an
HTTP response status+body; `route()` is `const` and returns `404` when no
route matches. The `data_store::Client*` is **not** a `route()` argument —
`install_handlers()` injects it by capturing it in each handler's closure
(§2.5), so the router and session stay decoupled from the data store.

```cpp
struct HttpResponse {
    int         status = 200;
    std::string content_type = "application/json";
    std::string body;
    std::string to_string() const;   // status-line + headers + body
};

class Router {
public:
    using HandlerFn = std::function<HttpResponse(const HttpParser::Request&)>;

    void add(std::string method, std::string path, HandlerFn fn);
    HttpResponse route(const HttpParser::Request& req) const;   // 404 if no match

private:
    using Key = std::pair<std::string, std::string>;  // (method, path)
    std::map<Key, HandlerFn> m_routes;
};

// handler.hpp — installs /api/v1/db/* on the router, binding `ds` into
// each handler closure. `ds` must outlive the router (owned by main).
void install_handlers(Router& router, data_store::Client* ds);
```

### 2.4 Long-poll support

`GET /api/v1/db/get?key=<name>&timeout=<seconds>`

The handler (a closure over `ds`, registered by `install_handlers`):
1. Reads the current value via `ds->get({key})`.
2. `timeout` absent or `0`: return the current value immediately
   (`changed:false`). `timeout` is clamped to `[0, 300]` seconds.
3. Otherwise `ds->watch(key)`, block on `ds->recv_event(ev, timeout*1000)`,
   then `ds->unwatch(key)`.
4. Event fired: `changed:true` with the new `value` (and `prev` when the
   previous value was a string). Timeout expired: `changed:false` with the
   primed current value.

```cpp
// install_handlers(): router.add("GET", "/api/v1/db/get", [ds](const Request& req){
HttpResponse h;
std::string key = req.query.at("key");
int timeout = clamp(atoi(req.query["timeout"]), 0, 300);

std::vector<Client::GetResult> got;
auto rs = ds->get({key}, got);            // prime
json resp; resp["key"] = key;

if (timeout == 0) { resp["changed"] = false; resp["value"] = ...; }
else {
    ds->watch(key);
    Client::Event ev;
    auto ev_rs = ds->recv_event(ev, timeout * 1000);   // BLOCKS this thread
    ds->unwatch(key);
    if (ev_rs.ok) { resp["changed"] = true; resp["value"] = ev.value; /* +prev */ }
    else          { resp["changed"] = false; resp["value"] = primed; }
}
h.body = resp.dump();
```

> **Concurrency.** `recv_event` blocks the thread that runs the handler.
> With `http.workers = 0` (default) that's the **reactor thread**, so an
> in-flight long-poll stalls all other connections. Set `http.workers = N`
> to run handlers on a worker pool (§2.8) — then a long-poll blocks only a
> worker, and the reactor keeps serving everyone else. `data_store::Client`
> is thread-safe, so the single shared client serves all workers.

### 2.5 Main loop (hand-rolled accept + ACE_Reactor)

v1 does **not** use `ACE_Acceptor` (it would require a default-constructible
session) nor a worker pool. `main()` opens a plain `ACE_SOCK_Acceptor` and
drives a single loop that polls for new connections and pumps the reactor:

```
main()
  ├── data_store::Client::connect(ds-socket)        // to ds-server
  ├── read http.listen.* / http.tls.* (CLI > ds)
  ├── install_handlers(router, &ds)                 // binds ds into closures
  ├── if scheme == https: TlsContext.load_server(cert, key, ca)
  ├── ACE_SOCK_Acceptor.open(addr)                  // listen on ip:port
  └── while (!stop):                                // SIGINT/SIGTERM set stop
        ├── accept(stream, &peer, 0)  // non-blocking poll
        │     └── new HttpSession(router, tlsPtr)
        │         → set_handle(fd) → reactor.register_handler(READ_MASK)
        └── reactor.handle_events(50ms)             // services session reads
```

The reactor services per-connection reads; the listening socket is polled
directly each 50 ms tick (not registered with the reactor). Everything —
accept, decrypt, parse, handler, encrypt, send — happens on this one
thread:

```
single reactor thread
─────────────────────
accept() → new HttpSession(router, tls?)
handle_events():
  handle_input() → [TLS decrypt] → parser.feed()
                 → parser fires handler INLINE → route() → handler(req)
                   └─ ds->get/set/watch (may block on long-poll)
                 → send_bytes(take_response())  [TLS encrypt]
```

With `http.workers > 0` the handler runs on a worker pool instead of
inline — see §2.8.

### 2.6 Configuration

`iot-httpd` reads its listen parameters from the data store at
startup via the `http.*` schema (shipped alongside `iot.lua`,
`services.lua`, etc. in `/etc/iot/ds-schemas/http.lua`):

```lua
-- http.listen.ip     = "0.0.0.0"   (default: all interfaces)
-- http.listen.port   = 8080        (default, 1..65535)
-- http.listen.scheme = "http"      ("http" | "https")
-- http.tls.cert      = ""          (PEM cert chain; required for https)
-- http.tls.key       = ""          (PEM private key; required for https)
-- http.tls.ca        = ""          (PEM CA bundle; non-empty → mTLS)
-- http.workers       = 0           (handler pool size; 0 = inline; §2.8)
```

CLI overrides (for development / smoke; CLI wins over the data store):

```sh
iot-httpd \
    ds-socket=/var/run/iot/data_store.sock \
    http-scheme=https \
    http-cert=/etc/iot/tls/server.crt \
    http-key=/etc/iot/tls/server.key \
    http-ca=/etc/iot/tls/clients-ca.crt \  # optional → mTLS
    http-workers=4                          # 0 = inline (default)
```

The server reads `http.listen.{ip,port,scheme}` and `http.tls.{cert,key,ca}`
at startup via `data_store::Client::get()`. If the keys are unset, schema
defaults apply. Hot-reload of listen/TLS params is FUP-L18-2 — changing
ip/port/cert requires a restart in v1.

### 2.7 TLS termination (https + mTLS)

With `http.listen.scheme = "https"`, `iot-httpd` terminates TLS itself —
no reverse proxy required (one can still front it; leave the scheme
`http`). The design mirrors the xpmile inner-TLS pattern: an OpenSSL `SSL`
object driven through a pair of **memory BIOs** rather than `SSL_set_fd`.
The session shuttles ciphertext between the BIOs and the existing
`ACE_SOCK_Stream`, so OpenSSL never performs socket I/O and never blocks
the reactor thread — the handshake advances incrementally as bytes
arrive, exactly like the plaintext path. No `ACE_SSL_SOCK_*`, no blocking
`SSL_accept`, no extra threads.

```
   socket (ciphertext)                         OpenSSL                app
   ──────────────────                          ───────                ───
   peer().recv() ───▶ feed_ciphertext() ─▶ [ rbio ] ─▶ SSL ─▶ read_plaintext() ─▶ parser
   peer().send_n() ◀── drain_outgoing() ◀─ [ wbio ] ◀─ SSL ◀─ write_plaintext() ◀─ response
```

**Two classes (`tls.hpp`/`tls.cpp`):**

```cpp
// Process-wide; one SSL_CTX shared by every connection.
class TlsContext {
    bool load_server(cert_path, key_path, ca_path = "");  // TLS 1.2 floor
    bool mtls() const;            // true when a CA was supplied
};

// Per-connection engine over two memory BIOs.
class TlsConn {
    void        feed_ciphertext(data, len);   // socket  → rbio
    int         handshake();                  // 1 done / 0 again / -1 fail
    int         read_plaintext(out);          // SSL_read loop
    int         write_plaintext(data, len);   // SSL_write → wbio
    std::size_t drain_outgoing(out);          // wbio    → socket
};
```

**`HttpSession::handle_input()` gains an optional TLS leg** (the plaintext
path is unchanged when `m_tls == nullptr`):

1. `recv()` bytes — ciphertext on https.
2. `feed_ciphertext()`; if the handshake isn't done, `handshake()` and
   flush `drain_outgoing()` to the peer; return early until it completes.
3. `read_plaintext()` → the decrypted request bytes → `parser.feed()`.
4. Response goes back through `write_plaintext()` + `drain_outgoing()`
   (unified behind `send_bytes()`, which is a straight `send_n()` on
   plain HTTP).

**Mutual-TLS.** A non-empty `http.tls.ca` switches `SSL_CTX_set_verify`
to `SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT`: clients must
present a certificate that chains to that CA, else the handshake is
rejected. The CA can be the iot fleet's existing openvpn CA
(`vpn.ca.path`, e.g. `/etc/iot/vpn/ca.crt`) for a single shared trust
domain, or a dedicated bundle to keep the VPN and HTTP identity sets
distinct. Hardening: TLS 1.2 floor (`SSL_CTX_set_min_proto_version`) and
`SSL_OP_NO_RENEGOTIATION`.

### 2.8 Handler thread pool (`http.workers`)

With `http.workers = N > 0`, handlers run on a pool of N threads instead
of inline on the reactor — the Half-Sync/Half-Async pattern. This stops a
blocking handler (notably a long-poll `recv_event`) from stalling every
other connection. `http.workers = 0` (default) keeps the original inline
behaviour.

`WorkerPool` (`worker.{hpp,cpp}`) is a plain std::thread pool with a
mutex/condvar job queue; `submit()` runs the job inline when size is 0, so
callers are agnostic. **All socket and TLS I/O stays on the reactor
thread** — workers only compute — so there are no TLS races and no
cross-thread socket writes:

```
reactor thread                         worker thread
──────────────                         ─────────────
handle_input(): parse request
  suspend_handler(this)  ──────────────▶ route(req) → m_response
                                          reactor.notify(this, EXCEPT_MASK)
handle_exception(): ◀──────────────────── (notify)
  send_bytes(m_response) [TLS encrypt]
  keep-alive → resume_handler(this)
  else       → return -1 → handle_close
```

Why this is safe without ACE reference counting:

- **One request in flight per connection.** On dispatch the session calls
  `suspend_handler(this)`, so the reactor fires no `handle_input` /
  `handle_close` on it while a worker owns it — the worker can't be racing
  a close, and responses stay ordered. `notify()` still reaches a
  suspended handler (it's not gated on fd readiness), so `handle_exception`
  runs.
- **Shared `m_response` is race-free** — exactly one writer (the worker)
  then one reader (the reactor), ordered by the notify.
- **Shared ds client is thread-safe** (its own listener thread owns the
  socket; `recv_event` blocks only the caller), so all workers share one
  client — no per-worker connection pool.
- **Shutdown** stops the pool (joins workers) before tearing down the
  reactor, so no worker is left holding a session.

Known limitation: concurrent long-polls share the Client's pull-style
event queue; per-watch isolation (callback-style) is a follow-up. The
reactor never blocks regardless.

## 3. REST API

### POST /api/v1/db/get

Read one or more keys from the data store.

**Request:**
```json
{
  "keys": ["iot.endpoint", "services.net.router.state"]
}
```

**Response (200):**
```json
{
  "ok": true,
  "data": {
    "iot.endpoint": "urn:dev:client-1",
    "services.net.router.state": "running"
  }
}
```

**Response (partial — key missing):**
```json
{
  "ok": true,
  "data": {
    "iot.endpoint": "urn:dev:client-1",
    "services.net.router.state": null
  }
}
```

### POST /api/v1/db/set

Write one or more key/value pairs. Values are typed per the
data-store schema.

**Request:**
```json
{
  "pairs": [
    {"key": "iot.endpoint", "value": "urn:dev:client-2"},
    {"key": "services.net.router.enable", "value": false}
  ]
}
```

**Response (200):**
```json
{
  "ok": true,
  "changed": 2
}
```

**Response (schema rejection):**
```json
{
  "ok": false,
  "err": "schema(services.ds.enable): namespace 'services' is claimed but this key is not declared"
}
```

### GET /api/v1/db/get?key=<name>&timeout=<seconds>

Long-poll: wait for a key to change. Returns immediately if
`timeout` is absent or 0.

**Query params:**
- `key` (required): full key name, e.g. `services.net.router.state`
- `timeout` (optional, default 0): max seconds to wait

**Response — immediate (timeout=0):**
```json
{
  "key": "services.net.router.state",
  "value": "running",
  "changed": false
}
```

**Response — changed within timeout:**
```json
{
  "key": "services.net.router.state",
  "value": "disabled",
  "prev": "running",
  "changed": true
}
```

**Response — timeout expired, no change:**
```json
{
  "key": "services.net.router.state",
  "value": "running",
  "changed": false
}
```

## 4. Module layout

```
modules/http-server/                 # new top-level module
├── CMakeLists.txt
├── inc/
│   └── http_server/
│       ├── parser.hpp               # HttpParser
│       ├── session.hpp              # HttpSession (ACE_Svc_Handler)
│       ├── router.hpp               # Router
│       ├── handler.hpp              # HttpResponse + handler fns
│       ├── tls.hpp                  # TlsContext + TlsConn (https/mTLS)
│       └── worker.hpp               # WorkerPool (handler thread pool)
├── src/
│   ├── main.cpp                     # entry point + acceptor + reactor
│   ├── parser.cpp
│   ├── session.cpp
│   ├── router.cpp
│   ├── handler.cpp                  # /api/v1/db/* handlers
│   ├── tls.cpp                      # OpenSSL memory-BIO TLS engine
│   └── worker.cpp                   # std::thread handler pool
├── test/
│   ├── parser_test.cpp              # HttpParser unit tests
│   ├── router_test.cpp              # Router dispatch tests
│   ├── handler_test.cpp             # API handler tests (needs ds-server)
│   ├── tls_test.cpp                 # handshake + echo + mTLS (OpenSSL-only)
│   └── worker_test.cpp              # WorkerPool tests (pure C++17)
└── schemas/
    └── http.lua                     # http.listen.* + http.tls.* + http.workers
```

## 5. Non-goals (v1)

- WebSocket upgrade
- HTTP/2
- Application-level authn/authz (transport client auth is available via
  mTLS — §2.7; per-key authorization still reuses the L17c ds-server ACL)
- Rate limiting (reuses L17d ds-server rate-limit)
- Static file serving
- Request logging to file (ACE_DEBUG to stderr is sufficient for v1)
- CORS headers

## 6. Design decisions

| ID | Decision | Rationale |
|----|----------|-----------|
| D1 | Handlers run **inline on the reactor thread** by default; an optional thread pool (`http.workers > 0`, §2.8) off-loads them | Inline is simplest and correct for light loads; the pool removes the long-poll-blocks-reactor problem (§2.4) without per-worker ds connections (the client is thread-safe). Socket/TLS I/O stays on the reactor for both |
| D2 | HttpParser is push-based (no pull/peek) | Matches reactor pattern — bytes arrive asynchronously, parser consumes what it can |
| D3 | One shared `data_store::Client*`, injected via handler closures | `install_handlers()` binds it into each route; the router/session stay decoupled from the data store. The unix socket is used serially from the single reactor thread, so no pooling is needed in v1 |
| D4 | JSON for both request and response bodies | Matches existing ds-cli/ds-server protocol; operators already know the shape |
| D5 | Long-poll uses pull-style watch (not callback) | Simpler — handler blocks on `recv_event()`, wakes on change or timeout. No callback threading issues |
| D6 | Body is `Content-Length`-delimited **or** `Transfer-Encoding: chunked`; absent → empty body (no `411`) | Both framings supported (chunked wins when both present, §2.1). A request with neither is parsed as a zero-length body rather than rejected — strict `411` enforcement is FUP-L18-3. Malformed request lines → `400` |
| D7 | Keep-alive opt-in per request | Default `Connection: close` reduces resource leaks from forgotten clients |
| D8 | TLS via OpenSSL memory BIOs, not `ACE_SSL_SOCK_*`/`SSL_set_fd` | Keeps the non-blocking reactor model — ciphertext is pumped between BIOs and the socket, so the handshake never blocks the reactor thread or needs a worker |
| D9 | mTLS = presence of `http.tls.ca` | One knob: a CA bundle both verifies clients and flips `SSL_VERIFY_FAIL_IF_NO_PEER_CERT`. Reuses the fleet's openvpn CA (`vpn.ca.path`) when a shared trust domain is wanted |
| D10 | Native TLS termination supersedes "reverse proxy only" | TLS 1.2 floor + no-renegotiation; a proxy is still valid (leave scheme `http`) but no longer required |

## 7. Follow-up items (FUP)

| ID | Item | Notes |
|----|------|-------|
| ~~FUP-L18-1~~ | **Worker pool for handlers — DONE** | Implemented as the `http.workers` thread pool (§2.8): handlers run off the reactor thread, the reactor sends via `handle_exception` after a worker `notify()`s, suspend/resume serialises one request per connection. Default 0 (inline). |
| FUP-L18-2 | Hot-reload of `http.listen.*` / `http.tls.*` | Read once at startup today; changing ip/port or rotating the TLS cert needs a restart (§2.6). Watch the keys and rebind / reload `SSL_CTX`. |
| FUP-L18-3 | Strict `411 Length Required` | Reject a body-bearing method that carries neither `Content-Length` nor `Transfer-Encoding: chunked`, instead of treating it as empty-body (D6). |
