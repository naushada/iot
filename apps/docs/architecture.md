# IoT LwM2M Stack — Current Architecture

Status: **as-implemented snapshot** (pre-ACE refactor). This document captures
the codebase as it stands so we have a fixed baseline to compare against
during the ACE refactor and the LwM2M design work that follows.

## 1. What the binary does

A single C++17 executable, `lwm2m`, that plays one of two roles selected at
startup:

| Role     | Sockets it owns                              | Purpose                                                |
|----------|----------------------------------------------|--------------------------------------------------------|
| `client` | one UDP socket bound to `local=host:port`    | LwM2M device. Connects to a bootstrap server at `bs=…` |
| `server` | one socket on `local=…` (bootstrap)          | LwM2M bootstrap server                                 |
|          | + one socket on `:5683` (device management)  | LwM2M registration / device-management server         |

The scheme is selected by the URI prefix: `coap://` runs unencrypted UDP,
`coaps://` runs DTLS-PSK via tinydtls. PSK credentials are supplied as
`identity=` / `secret=` (secret is a hex-encoded 128-bit key).

## 2. Layered view

```
                   main.cpp
                      │
                      ▼
                    App ──► UDPAdapter ──► ServiceContext (per socket)
                                                  │
                            ┌─────────────────────┼─────────────────────┐
                            ▼                     ▼                     ▼
                       DTLSAdapter           CoAPAdapter           LwM2MAdapter
                       (tinydtls)            (RFC 7252)           (OMA TLV / objects)
                                                  │
                                                  ▼
                                         CBORAdapter / zlib
                                       (payload codecs: uCBOR,
                                        uCBORZ, SUCBOR, SUCBORZ,
                                        TS, LwM2M+TLV)
```

External:

- `Readline` — GNU readline shell exposed only in client mode for typing
  CoAP requests (`post uri="/bs" uri-query="ep=…"`).
- `DbClient` — MongoDB GridFS / collection helpers (declared but not yet
  wired into the runtime path).

## 3. File map

| File | Lines | Role |
|------|------:|------|
| `src/main.cpp` | 211 | CLI parsing, App bootstrap, signal masks, mode dispatch |
| `src/app.cpp`, `inc/app.hpp` | 14 + 59 | Thin façade; holds `LwM2MBootstrapState` / `LwM2MDeviceManagementState` enums (not yet driven) |
| `src/udp_adapter.cpp`, `inc/udp_adapter.hpp` | 379 + 192 | `epoll` reactor, per-service socket context, scheme dispatch |
| `src/dtls_adapter.cpp`, `inc/dtls_adapter.hpp` | 287 + 298 | tinydtls wrapper, four C callbacks, PSK store, per-peer `ClientDetails` |
| `src/coap_adapter.cpp`, `inc/coap_adapter.hpp` | 1306 + 317 | Hand-rolled CoAP parser/builder, option/Block1 codec, LwM2M URI routing, content-format codecs |
| `src/lwm2m_adapter.cpp`, `inc/lwm2m_adapter.hpp` | 1795 + 214 | OMA Object model, TLV serialise/parse, security/server bootstrap payload from JSON |
| `src/cbor_adapter.cpp`, `inc/cbor_adapter.hpp` | 80 + 71 | nlohmann::json CBOR wrapper |
| `src/db_adapter.cpp`, `inc/db_adapter.hpp` | 948 + 101 | MongoDB pool, GridFS upload/download, document CRUD (unused at runtime) |
| `src/readline.cpp`, `inc/readline.hpp` | 516 + 122 | Interactive client-side shell, tab completion |

Config:

- `apps/config/securityObject/{0,1,modified1}.json` — Security Object (OID 0)
  bootstrap payloads.
- `apps/config/serverObject/{0,1}.json` — Server Object (OID 1) bootstrap
  payloads.
- `apps/config/deviceObject/0.json` — Device Object (OID 3) per-RID
  overrides for the static metadata (Manufacturer, Model, Serial,
  Firmware Version, …). Compiled-in defaults apply per-RID when this
  file is missing or a particular RID is absent / `include: false`. See
  RDD D6.

The L1–L8 LwM2M build adds the modules in `apps/inc/lwm2m_*.hpp`
(object store, TLV / SenML / link-format / plain text / opaque codecs,
codec registry, registration + bootstrap FSMs, DM client + server,
observe engine, canonical-object installers). See `lwm2m-design.md`
§3.1 / §8 for the module map.

## 4. Process / threading model

- **One process per binary.** Plain reactor; no fork.
- **Server**: single thread. `main` calls `App::start` → `UDPAdapter::start`
  → blocking `epoll_wait` loop.
- **Client**: two threads.
  - Worker: `std::thread reception_thread(&App::start, app, …)` runs the
    same `epoll_wait` loop, consuming inbound DTLS / CoAP traffic.
  - Main: `Readline::start()` reads user commands and pushes outbound
    requests through `CoAPAdapter::buildRequest` → `DTLSAdapter::tx`.

No mutex is held around `DTLSAdapter` state today. The reader thread writes
to `m_responses` / `m_session` while the readline thread can call
`DTLSAdapter::tx`; this is a known issue to address in the refactor.

## 5. I/O reactor (`UDPAdapter`)

`UDPAdapter::init` creates a UDP socket, binds to `host:port`, and stores a
`ServiceContext_t` in `m_services` keyed by `ServiceType_t`
(`DeviceMgmtServer`, `BootstrapServer`, `DeviceMgmtClient`, `LwM2MClient`).

`add_event_handle` registers the fd with `epoll_ctl(EPOLL_CTL_ADD, … EPOLLIN | EPOLLHUP)`
and packs the routing key into `epoll_event.data.u64`:

```
[ fd:32 | service:16 | scheme:16 ]
```

so `handle_io` unpacks it without an extra lookup:

```cpp
std::int32_t handle = (data.u64 >> 32) & 0xFFFFFFFF;
ServiceType_t svc   = static_cast<ServiceType_t>((data.u64 >> 16) & 0xFFFF);
Scheme_t scheme     = static_cast<Scheme_t>(data.u64 & 0xFFFF);
```

`handle_io` then dispatches on scheme:

- `Scheme_t::CoAP` → `handle_io_coap`: `recvfrom` → `CoAPAdapter::parseRequest`
  (potentially looping on Block1 `ismorebitset`) → `processRequest` →
  `sendto` for each response.
- `Scheme_t::CoAPs` → `handle_io_coaps`: `DTLSAdapter::rx(fd)` which calls
  `dtls_handle_message`; tinydtls fires `dtlsReadCb` with plaintext which in
  turn calls `CoAPAdapter::processRequest`; the resulting response list is
  shipped via `DTLSAdapter::tx` (which encrypts and `sendto`s).

`epoll_wait` timeout is 9000 ms; the timeout branch is empty today (no
periodic ticks).

## 6. DTLS plumbing (`DTLSAdapter` + tinydtls)

tinydtls calls four C callbacks; each retrieves the `DTLSAdapter` instance
from `dtls_get_app_data(ctx)`.

| Callback | Direction | What it does |
|----------|-----------|---|
| `dtlsWriteCb` | egress | `sendto(fd, ciphertext, …)` |
| `dtlsReadCb`  | ingress | Wraps plaintext, calls `CoAPAdapter::processRequest`, stashes responses in `m_responses` |
| `dtlsEventCb` | events  | Maintains per-peer `ClientDetails` (server) or scalar `clientState` (client) |
| `dtlsGetPskInfoCb` | PSK | Returns identity (via `identity()`) and binary secret (via `get_secret` + `hexToBinary`) |

`connect()` calls `dtls_connect`; `rx()` calls `dtls_handle_message`; `tx()`
calls `dtls_write`. The session is held in `m_session` as a single client
instance (no per-peer session map on the client side).

## 7. CoAP layer (`CoAPAdapter`)

Hand-written; no libcoap. Highlights:

- Lookup tables for option numbers, content-formats (incl. LwM2M-specific
  `application/vnd.oma.lwm2m+tlv` = 11542 and custom uCBOR/uCBORZ/SUCBOR
  codes 12200–12203), method codes, response codes.
- `parseRequest` (line 707) decodes header + token + variable-width options
  + Block1 chunked reassembly via `ismorebitset`.
- Option-delta accumulation goes through `SumOptionNumber` (reset before
  each message by `resetCummulativeOptionNumber()`).
- `processRequest` (lines 1019 / 1139) is the policy core. URI classification:
  - `isCoAPUri(message, uri)` — matches `set | get | push | execute` (client-side shell).
  - `isLwm2mUri(message, …)` — matches `rd` / `bs` or returns object-path triple (oid, oiid, rid, riid).
- LwM2M routes:
  - `/bs` on a server → `buildRegistrationAck` + `handleLwM2MObjects` (push Security + Server bootstrap objects).
  - `/rd` → `buildRegistrationAck` only.
  - Numeric object path → parse TLV via `LwM2MAdapter::parseLwM2MObjects`.
- Content-format routes for non-LwM2M payloads:
  - 12201 uCBORZ → zlib `uncompress` + CBOR decode, write to `ucborz_cf_12201.txt`.
  - 12200 uCBOR → CBOR decode straight, write to `ucbor_cf_12200.txt`.
  - 12202/12203 SUCBOR/Z → file dump.
  - 12119 TS → file dump.
  - 11542 LwM2M+TLV → `LwM2MAdapter::parseLwM2MObjects`.

## 8. LwM2M layer (`LwM2MAdapter`)

Encodes the OMA object model. Each object has a `<rid → name>` and
`<name → rid>` map seeded in the constructor for OID 0 (Security), 1 (Server),
2 (Access Control), 3 (Device), 4 (Connectivity Monitoring),
5 (Firmware Update), 6 (Location), 7 (Connectivity Statistics).

TLV serialise/parse follows OMA-TS-LightweightM2M Annex C:

- Type bits 7-6: `{ObjectInstance, ResourceInstance, MultipleResource, ResourceWithValue}`.
- Type bit 5: identifier length (8b / 16b).
- Type bits 4-3: payload-length field width (0 / 8b / 16b / 24b).

`bootstrapSecurityObject00`, `devicemgmtSecurityObject01`, `serverObject30`,
`securityObject` build the binary TLV blob from the JSON files under
`apps/config/`.

## 9. Build inputs

`apps/CMakeLists.txt`:

- C++17, `-Wall -Wextra -O0 -g`, `-DLWM2M_LITTLE_ENDIAN`.
- Includes: `3rdparty/tinydtls`, `3rdparty/json/single_include`,
  `/usr/local/include/{mongocxx,bsoncxx}/v_noabi`, project `inc/`.
- Sources: `file(GLOB SOURCES "src/*.cpp")`.
- Links: `libtinydtls.a` (static), `z`, `readline`, `pthread`, `bsoncxx`,
  `mongocxx`, `ssl`, `crypto`.
- `add_subdirectory(test)` builds the GoogleTest binary `lwm2m_test`.
- Install target installs `lwm2m` to `/usr/local/bin` on `Release` builds.

`docker/Dockerfile`:

- Base `ubuntu:jammy`.
- Builds OpenSSL 3.1.1 from source, mongo-c r1.19 + mongo-cxx v3.6.
- Clones the project, builds `3rdparty/tinydtls` via autoconf, then
  `cmake .. && make` in `apps/build/`.
- Copies `lwm2m` and `lwm2m_test` to `/opt/app/`.

## 10. End-to-end flow (CoAPs client bootstrap)

1. CLI parsed → `App` + `UDPAdapter` constructed → UDP socket created and
   bound to `local`.
2. `dtlsAdapter->add_credential(identity, secret)` populates the PSK store.
3. `UDPAdapter::start` → `DTLSAdapter::connect` → tinydtls emits ClientHello
   via `dtlsWriteCb`.
4. Server replies with HelloVerifyRequest; client retries ClientHello with
   cookie; full PSK handshake follows (TLS_PSK_WITH_AES_128_CCM_8 as seen in
   the captured logs).
5. `dtlsEventCb` fires `DTLS_EVENT_CONNECTED` → `clientState("connected")`.
6. User types `post uri="/bs" uri-query="ep=A123…"` in the readline shell
   → `CoAPAdapter::buildRequest` / `serialisePOST` → `DTLSAdapter::tx`.
7. Server decrypts, `parseRequest` → `processRequest` matches `/bs` →
   replies with 2.04 Changed ACK, then pushes Security (OID 0) and Server
   (OID 1) TLV objects.
8. Client receives ACK + objects, decodes TLV, stores them.

## 11. Captured log evidence (`log/dtlsc.txt`, `log/dtlss.txt`)

Both logs are from a single run on 2024-04-09 at 12:24:47 on
`192.168.0.181`. They capture **only the DTLS handshake**; no CoAP/LwM2M
exchange follows. Concretely:

- Both reach `Handshake complete` / `Peer is connected`.
- Cipher suite is `TLS_PSK_WITH_AES_128_CCM_8`, master/key block derivation
  visible.
- Identity used: `97554878B284CE3B727D8DD06E87659A` (32-byte hex string sent
  on the wire; not the 16-byte binary form).
- PSK secret: `3894BEED…A59525` (16 bytes binary after `hexToBinary`).
- Server tracks `192.168.0.181:56830` as a new peer through
  `client_hello → hello_verify_request → client_hello+cookie → server_hello,
  server_hello_done → client_key_exchange → finished` and emits its own
  Finished.

The logs end immediately after handshake completion — there is no captured
`/bs` POST or registration traffic — meaning the runtime behavior above
handshake has not been exercised in these traces. We'll need new captures
once the ACE refactor lands to validate LwM2M flows.

## 12. Known gaps / smells (carry into refactor)

- `UDPAdapter::process_request` is dead code (called from a now-commented
  path); routing happens inside `handle_io_coap` / `handle_io_coaps` instead.
- `App::start/stop` and the `LwM2MBootstrapState` / `LwM2MDeviceManagementState`
  enums exist but no state machine is driven from them.
- Client side keeps a single `m_session`; multi-server topologies are not
  modeled.
- No locking around `DTLSAdapter::m_responses` between the readline thread
  and the reactor thread.
- `epoll_wait` timeout returns are unused — no LwM2M lifetime / registration-
  update ticker yet.
- `DbClient` is built and linked but the request path never touches it.
- Error handling is largely `std::cout` log and continue; no recovery on
  socket errors.
- `coap_adapter.cpp::processRequest` has two overloads sharing ~90% of the
  logic — candidate for consolidation.

These are the load-bearing items to keep in mind when designing the ACE
replacement.

## 13. Post-L9 layout (2026-05-29)

After the L1–L9 build-out the runtime is structured as:

```
main.cpp
  ├─ wire_server(app, configDir)        L9
  │    ├─ ClientRegistry                L3
  │    ├─ RegistrationServer            L3   → CoAPAdapter::registrationServer()
  │    ├─ bootstrap::Server             L4   → CoAPAdapter::bootstrapServer()
  │    │    └─ AccountProvisioning loaded from apps/config/{security,server}Object/*.json
  │    └─ on_tick_server → registry->expire(now)        REQ-IO-005
  │
  └─ wire_client(app, ep, configDir, bsHost, bsPort)    L9
       ├─ ObjectStore                   L1
       │    └─ install_canonical_objects(store, configDir)        L8
       │         ├─ install_device      L8  (reads apps/config/deviceObject/0.json per D6)
       │         ├─ install_connmon     L8
       │         ├─ install_location    L8
       │         └─ install_connstats   L8
       ├─ DmClient                      L5  → CoAPAdapter::dmClient()
       │    └─ ObserverRegistry         L7  (NON default, every-10th CON per D4)
       ├─ bootstrap::Client              L4  → CoAPAdapter::bootstrapClient()  L9
       ├─ RegistrationClient            L3  (driven from on_tick_client)
       └─ on_tick_client → reg->should_send_update + dm->tick(now)  REQ-IO-005

Codec layer (used by all of the above):
  ├─ TLV                                L1
  ├─ link-format                        L2
  ├─ Plain text + Opaque                L5
  ├─ SenML JSON + SenML CBOR            L6
  └─ CodecRegistry (CF lookup)          L1 / extended L6
```

The Leshan interop test plan lives at `apps/docs/leshan-interop.md`;
the harness is `docker/docker-compose.leshan.yml`. Memory directory
under `~/.claude/projects/-Users-naushada-repo-iot/memory/` records the
six binding decisions (D1 spec version, D2 short-server-id keying,
D3 registry storage, D4 notify transport, D5 push plane, D6 device
backing).
