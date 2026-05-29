# ACE Refactor Design

Replace the epoll/`std::thread`/raw-socket I/O layer with ACE primitives so
the IoT stack uses the same I/O conventions as xpmile
(`/usr/local/ACE_TAO-7.0.0`). The CoAP/LwM2M/CBOR/DB adapters are **not**
touched in this change.

## 1. Decisions

| Decision                       | Choice                                                                                  |
|--------------------------------|-----------------------------------------------------------------------------------------|
| DTLS ↔ Reactor coupling        | Per-peer state behind a single `ACE_Event_Handler` per UDP socket (see §3)              |
| Refactor scope                 | Sockets + reactor + thread model + DTLS adapter clean-up                                |
| Delivery                       | Single PR replacing UDPAdapter / DTLSAdapter I/O entry points                           |
| ACE install path               | `/usr/local/ACE_TAO-7.0.0` (matches xpmile)                                             |
| Compatibility shim             | None — the old `epoll`/`std::thread` paths are deleted                                  |

## 2. xpmile parallels we will copy

From `xpmile/modules/module/webservice/`:

- `WebServer : public ACE_Event_Handler` listens on a socket, registers with
  `ACE_Event_Handler::ACCEPT_MASK | SIGNAL_MASK`. → **IoT: `UdpService`** with
  `READ_MASK | SIGNAL_MASK` (no acceptor; UDP is connectionless).
- `WebConnection : public ACE_Event_Handler` is created on accept and owns
  per-connection state. → **IoT: `DtlsPeer`**, but it does **not** register
  with the reactor (it has no unique fd — see §3).
- `MicroService : public ACE_Task<ACE_MT_SYNCH>` worker pool consumes
  message queue. → **IoT: `ReactorTask : public ACE_Task<ACE_MT_SYNCH>`**
  whose `svc()` runs the reactor event loop on the client side.
- `ACE_Reactor::instance()->handle_events(to)` loop, signal subscription via
  `ACE_Sig_Set`, structured `ACE_DEBUG((LM_DEBUG, ACE_TEXT("…")))` logging.

## 3. UDP + DTLS specifics: per-peer handlers without per-peer fds

UDP is *one fd, many peers*. ACE_Reactor dispatches by fd, so the "per-peer
Event_Handler" we agreed on lives behind a session table rather than the
reactor directly:

```
ACE_Reactor
    │
    ▼
UdpService (Event_Handler, owns ACE_SOCK_Dgram on one fd)
  │   handle_input(): recv_from → peer addr → look up / create DtlsPeer
  │                                          → DtlsPeer::on_datagram(bytes)
  │
  ├──► DtlsPeer A   (owns tinydtls session, CoAP context, peer addr)
  ├──► DtlsPeer B
  └──► DtlsPeer …
```

- `UdpService` owns the UDP `ACE_SOCK_Dgram` (or `ACE_SOCK_Dgram` opened on
  bind) and one `dtls_context_t` if `scheme == CoAPs`.
- Inbound: `handle_input` calls `recv` (gives sender addr), looks up the
  `DtlsPeer` by peer address, and forwards the ciphertext to
  `DtlsPeer::on_ciphertext` which calls `dtls_handle_message`. tinydtls then
  invokes the read callback with plaintext — the read callback retrieves
  the per-peer `DtlsPeer` from the tinydtls `session_t->addr` mapping.
- Outbound: `DtlsPeer::send(plaintext)` calls `dtls_write`; tinydtls fires
  the write callback which calls `UdpService::send_raw(peer_addr, bytes)`.

The bootstrap server creates `DtlsPeer` lazily on the first
`DTLS_EVENT_CONNECT` for a new address. This matches today's
`DTLSAdapter::m_clients` map, except the entry now carries the per-peer
session state instead of just a status tag.

### Why not a separate handler per peer?

ACE registers handlers by fd. With a single UDP fd, registering N handlers
either (a) doesn't work because the reactor dispatches the first match, or
(b) requires opening a new ephemeral connected-UDP socket per peer (à la
`connect()` on UDP) which complicates the bootstrap server's bind on `:5684`.
The session-table approach is the conventional pattern for UDP servers in
ACE.

## 4. Client thread model

| Component       | Thread                                      | Notes                                                  |
|-----------------|---------------------------------------------|--------------------------------------------------------|
| `ReactorTask`   | `ACE_Task<ACE_MT_SYNCH>::svc()`             | Runs `ACE_Reactor::instance()->handle_events()` loop   |
| `Readline`      | Main thread                                 | Blocks in GNU readline; produces outbound CoAP frames  |
| Inbound bridge  | tinydtls callbacks in reactor thread        | Already correct — fired on the reactor thread          |
| Outbound bridge | `ACE_Reactor::instance()->notify(handler)`  | Readline thread posts work into the reactor thread     |

`notify()` is the key mechanism: tinydtls is **not thread-safe**, so every
`dtls_write` / `dtls_handle_message` must execute on the reactor thread.
Readline calls `ReactorTask::send_async(svc, payload)` which queues the
payload onto a `ACE_Thread_Mutex`-protected deque and calls
`reactor()->notify(this)`. The reactor thread then drains the deque inside
`handle_exception` and issues `DtlsPeer::send` for each payload.

This fixes the existing race on `DTLSAdapter::m_responses` (architecture
doc §12).

Server side has no readline → reactor is just `ACE_Reactor::instance()->handle_events()` in `main`.

## 5. Class layout (after refactor)

```cpp
// New file boundaries; existing class names retained where viable.

class UdpService : public ACE_Event_Handler {
  // Replaces UDPAdapter + ServiceContext_t (split out fd-owning bits).
  // One instance per bound UDP port.
  ACE_HANDLE get_handle() const override;
  int handle_input(ACE_HANDLE) override;
  int handle_signal(int, siginfo_t*, ucontext_t*) override;
  int handle_close(ACE_HANDLE, ACE_Reactor_Mask) override;
  int handle_exception(ACE_HANDLE) override;  // drains tx queue from notify()

  ACE_SOCK_Dgram m_sock;
  ACE_INET_Addr  m_self;
  Scheme_t       m_scheme;
  ServiceType_t  m_service;
  std::shared_ptr<DTLSContext> m_dtls;          // when scheme == CoAPs
  std::shared_ptr<CoAPAdapter> m_coap;
  std::unordered_map<std::string, std::shared_ptr<DtlsPeer>> m_peers; // key: "ip:port"

  // Outbound queue (readline → reactor):
  ACE_Thread_Mutex m_txMutex;
  std::deque<std::pair<ACE_INET_Addr, std::string>> m_tx;
};

class DtlsPeer {
  // Was DTLSAdapter::ClientDetails + per-peer session state.
  // Not an Event_Handler; lives in UdpService::m_peers.
  void on_ciphertext(const uint8_t* data, size_t len);
  void send_plaintext(const std::string& bytes);
  ACE_INET_Addr addr() const;
  session_t&    session();
  std::string&  ep();
  // … endpoint, lifetime, ts, state from today's ClientDetails …
};

class DTLSContext {
  // Was DTLSAdapter (PSK + tinydtls context).
  // Single instance shared across DtlsPeers on the same UdpService.
  dtls_context_t* ctx();
  std::string get_secret(const std::string& identity);
  bool         match_identity(const std::string& in, std::string& out);
  void         add_credential(const std::string& identity, const std::string& secret);
};

class ReactorTask : public ACE_Task<ACE_MT_SYNCH> {
  // Client-side: runs the reactor loop on its own thread.
  int open(void* = nullptr) override;   // activate() with 1 thread
  int svc() override;                   // handle_events loop until m_stop
  std::atomic<bool> m_stop;
};
```

`App::start(role, scheme)` is replaced by:

- Server: `ACE_Reactor::instance()->handle_events()` loop on the main
  thread (after each `UdpService` registers itself).
- Client: `ReactorTask::open()` to start the reactor thread, then the main
  thread enters `Readline::start()`.

## 6. tinydtls C-callback bridge

The four C callbacks remain, but now retrieve the **`UdpService`** instance
via `dtls_get_app_data(ctx)`. They use the peer's `session_t.addr` to look
up the correct `DtlsPeer`:

```cpp
int dtlsWriteCb(dtls_context_t* ctx, session_t* sess,
                uint8* data, size_t len) {
  auto* svc = static_cast<UdpService*>(dtls_get_app_data(ctx));
  ACE_INET_Addr to;
  to.set_addr(&sess->addr, sess->size);
  return svc->send_raw(to, data, len);     // ACE_SOCK_Dgram::send
}

int dtlsReadCb(dtls_context_t* ctx, session_t* sess,
               uint8* data, size_t len) {
  auto* svc = static_cast<UdpService*>(dtls_get_app_data(ctx));
  auto peer = svc->lookup_peer(sess);
  return peer->on_plaintext(data, len);    // → CoAPAdapter::processRequest
}
```

This removes the current single-session assumption in `DTLSAdapter::m_session`.

## 7. Build + container changes

**`apps/CMakeLists.txt`**: add ACE include / lib dirs and link `ACE`:

```cmake
include_directories(/usr/local/ACE_TAO-7.0.0/include)
link_directories(/usr/local/ACE_TAO-7.0.0/lib)
target_link_libraries(lwm2m … ACE)
```

`-DACE_HAS_CPP17 -DACE_USES_WCHAR=0` are inherited from the ACE headers;
no project-level defines needed.

**`docker/Dockerfile`**: add an ACE_TAO 7.0.0 build stage between OpenSSL
and tinydtls. Use the same prefix xpmile uses so the include / link paths
match in CI.

## 8. What stays untouched

- `coap_adapter.*` — pure logic, no socket calls.
- `lwm2m_adapter.*`, `cbor_adapter.*` — pure logic.
- `db_adapter.*` — unused at runtime; defer.
- `readline.*` — only its `tx` path changes to call `ReactorTask::send_async`
  instead of `DTLSAdapter::tx` directly.
- `test/` — built against CoAP / LwM2M adapters; should keep compiling.

## 9. Validation plan

ACE is not on the local dev box. Validation will happen in the Docker
image (where ACE_TAO is installed by the updated Dockerfile):

1. `docker build -t naushada/iot:ace .` builds clean.
2. Server: `lwm2m local=coap://0.0.0.0:5684 role=server` boots, binds.
3. Client: `lwm2m local=coap://0.0.0.0:56830 bs=coap://server:5684 role=client`
   reaches the readline prompt and `post uri="/bs" uri-query="ep=…"` round-trips.
4. Same for `coaps://` after rebuilding tinydtls.
5. Compare new packet captures with `log/dtlsc.txt` / `log/dtlss.txt` to
   verify the handshake bytes are byte-identical (tinydtls behavior must
   not have changed).

## 10. Risks

- **Compile risk**: ACE templates surface a lot of diagnostics; the first
  build is likely to need small edits.
- **Reactor signal handling**: ACE installs its own SIGCHLD/SIGPIPE handlers
  by default; main.cpp's manual `sigaction` is replaced with
  `ACE_Sig_Set` + `register_handler(&ss, this)`.
- **GNU readline + ACE_Task**: readline is blocking and not aware of the
  reactor; pressing Ctrl-C while a notify is in flight needs a careful test.
- **tinydtls thread-safety**: the design assumes *all* dtls_* calls happen on
  the reactor thread. The notify() bridge enforces this; any code that
  bypasses it is a bug.
