# L18 Design — HTTP REST API for the data store

> ACE-backed HTTP/1.1 server with in-house parser, Active Object
> worker pool, and long-poll support. Mirror of the xpmile
> UniService pattern applied to the iot data store.

## 1. Architecture

```
                          ┌──────────────────────────────────────┐
                          │           iot-httpd                   │
                          │                                      │
  HTTP client ──────────▶ │  ACE_Acceptor                        │
  (curl, browser,         │    │                                 │
   script)                │    ▼                                 │
                          │  HttpSession (ACE_Svc_Handler)       │
                          │    │ register with reactor            │
                          │    │ handle_input() → read bytes     │
                          │    ▼                                 │
                          │  HttpParser (in-house, push-based)    │
                          │    │ feed bytes → callback on complete│
                          │    ▼                                 │
                          │  Router: match method+path            │
                          │    │                                 │
                          │    ├── POST /api/v1/db/get           │
                          │    ├── POST /api/v1/db/set           │
                          │    └── GET  /api/v1/db/get?timeout=N │
                          │    │                                 │
                          │    ▼                                 │
                          │  Worker Pool (ACE_Task)              │
                          │    │ enqueue Request + data_store     │
                          │    │ Client → ds-server unix socket  │
                          │    ▼                                 │
                          │  Response: HTTP/1.1 + JSON body      │
                          │                                      │
                          │  data_store::Client (reuse)          │
                          │    │ unix socket → ds-server         │
                          └────┼──────────────────────────────────┘
                               │
                    ┌──────────▼──────────┐
                    │     ds-server        │
                    │  /var/run/iot/       │
                    │  data_store.sock     │
                    └─────────────────────┘
```

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
Chunked transfer encoding is NOT supported in v1 — all requests
must carry `Content-Length`.

### 2.2 HttpSession (ACE_Svc_Handler)

One per TCP connection. Registered with `ACE_Reactor` for
`READ_MASK`. On `handle_input()`:

1. `recv()` up to 4096 bytes into a buffer
2. Feed bytes to `HttpParser::feed()`
3. When `parser.done()`:
   - Route request → handler
   - `send()` HTTP response
   - If `Connection: keep-alive`: `parser.reset()`, stay registered
   - Else: close

```cpp
class HttpSession : public ACE_Svc_Handler<ACE_SOCK_Stream, ACE_MT_SYNCH> {
public:
    HttpSession(data_store::Client& ds, const Router& router);

    int handle_input(ACE_HANDLE fd) override;
    int handle_close(ACE_HANDLE fd, ACE_Reactor_Mask mask) override;

private:
    data_store::Client& m_ds;
    const Router&       m_router;
    HttpParser          m_parser;
    std::string         m_recv_buf;
};
```

### 2.3 Router

Minimal path dispatcher. Matches `(method, path)` to handler
functions. The handler receives the parsed request + a
`data_store::Client&` and returns an HTTP response status+body.

```cpp
struct HttpResponse {
    int         status = 200;
    std::string content_type = "application/json";
    std::string body;
};

class Router {
public:
    using HandlerFn = std::function<HttpResponse(
        const HttpParser::Request&, data_store::Client&)>;

    void add(std::string method, std::string path, HandlerFn fn);
    HttpResponse route(const HttpParser::Request& req,
                       data_store::Client& ds);

private:
    // (method, path) → handler
    std::map<std::pair<std::string,std::string>, HandlerFn> m_routes;
};
```

### 2.4 Long-poll support

`GET /api/v1/db/get?key=<name>&timeout=<seconds>`

The handler:
1. Reads the current value via `data_store::Client::get()`
2. If the key exists and `timeout=0` (or absent): return immediately
3. If `timeout=N` and the key exists: register a one-shot watch,
   block on `recv_event()` up to N seconds
4. If the watch fires (value changed): return the new value
5. If the timeout expires: return `{"changed":false}`

```cpp
HttpResponse handle_long_poll(const HttpParser::Request& req,
                               data_store::Client& ds) {
    std::string key = query_param(req, "key");
    int timeout = query_param_int(req, "timeout", 0);

    // Prime: get current value
    std::vector<Client::GetResult> got;
    ds.get({key}, got);

    if (timeout == 0) {
        // Immediate return
        return json_response(got);
    }

    // Long poll: watch + wait
    ds.watch(key);  // pull-style
    Client::Event ev;
    auto rs = ds.recv_event(ev, timeout * 1000);
    ds.unwatch(key);

    if (rs.ok) {
        return json_response({{"changed", true}, {"value", ev.value}});
    } else {
        return json_response({{"changed", false}});
    }
}
```

### 2.5 Main loop (ACE_Reactor + ACE_Task)

```
main()
  ├── data_store::Client::connect(ds-socket)    // to ds-server
  ├── Router::add("POST", "/api/v1/db/get", ...)
  ├── Router::add("POST", "/api/v1/db/set", ...)
  ├── Router::add("GET",  "/api/v1/db/get", ...)  // long-poll variant
  ├── ACE_Acceptor::open(addr)                   // listen on :8080
  ├── WorkerPool (ACE_Task, N threads)           // for CPU-bound handlers
  └── ACE_Reactor::run_reactor_event_loop()      // blocks until SIGTERM
```

The reactor handles:
- Accept: new connection → new HttpSession → register READ_MASK
- Read: bytes arrive → parser → handler → response → send
- Timeout: long-poll `recv_event` timeout → return unchanged response

The ACE_Task worker pool handles blocking handlers (e.g., a handler
that needs to call ds-server and wait). But since `data_store::Client`
is synchronous (send+recv on unix socket), simple handlers can run
on the reactor thread directly. Long-poll handlers MUST run on a
worker thread because `recv_event` blocks the calling thread.

**Decision (D1):** All handlers enqueue onto the ACE_Task pool.
The reactor thread only does accept + recv + parse. Handler
execution is fully asynchronous. Response is sent from the worker
thread via the session's send() (ACE_Svc_Handler::peer().send() is
thread-safe with ACE_MT_SYNCH).

```
Reactor thread            Worker thread
─────────────            ─────────────
accept()
recv() → parser.feed()
parser.done() → enqueue ──▶ dequeue
                              route() → handler(ds)
                              ds.set(...) / ds.get(...)
                              session->send(response)
                              return
```

### 2.6 Configuration

```sh
iot-httpd \
    http-listen=0.0.0.0:8080 \
    ds-socket=/var/run/iot/data_store.sock \
    worker-threads=4 \
    max-connections=128
```

Defaults: listen on `0.0.0.0:8080`, 4 worker threads, unlimited
connections (gated by ulimit).

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
│       └── handler.hpp              # HttpResponse + handler fns
├── src/
│   ├── main.cpp                     # entry point + acceptor + reactor
│   ├── parser.cpp
│   ├── session.cpp
│   ├── router.cpp
│   └── handler.cpp                  # /api/v1/db/* handlers
├── test/
│   ├── parser_test.cpp              # HttpParser unit tests
│   ├── router_test.cpp              # Router dispatch tests
│   ├── handler_test.cpp             # API handler tests
│   └── integration_test.cpp         # end-to-end with ds-server
└── schemas/                         # (empty — uses data-store schemas)
```

## 5. Non-goals (v1)

- TLS (HTTPS) — use a reverse proxy (nginx/haproxy) for TLS termination
- WebSocket upgrade
- Chunked transfer encoding
- HTTP/2
- Authentication (reuses unix socket DAC + future L17c ACL)
- Rate limiting (reuses L17d ds-server rate-limit)
- Static file serving
- Request logging to file (ACE_DEBUG to stderr is sufficient for v1)
- CORS headers

## 6. Design decisions

| ID | Decision | Rationale |
|----|----------|-----------|
| D1 | All handlers run on ACE_Task worker threads | Keeps reactor thread free for I/O; long-poll blocks a worker, not the acceptor |
| D2 | HttpParser is push-based (no pull/peek) | Matches reactor pattern — bytes arrive asynchronously, parser consumes what it can |
| D3 | `data_store::Client` is per-request, not pooled | Client already supports concurrent use from multiple threads; connection is a unix socket (low overhead) |
| D4 | JSON for both request and response bodies | Matches existing ds-cli/ds-server protocol; operators already know the shape |
| D5 | Long-poll uses pull-style watch (not callback) | Simpler — handler blocks on `recv_event()`, wakes on change or timeout. No callback threading issues |
| D6 | No `Content-Length` → `411 Length Required` | Chunked TE not supported in v1; strict enforcement avoids parser ambiguity |
| D7 | Keep-alive opt-in per request | Default `Connection: close` reduces resource leaks from forgotten clients |
