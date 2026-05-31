# L12 Plan — OpenVPN Client (ACE + data-store backed)

> Forward-looking phase plan. Mirrors L10/L11 shape. Greenfield
> `modules/openvpn-client/` module wrapping the upstream `openvpn(8)`
> binary via its management interface. Config in + state out flows
> through the same `ds-server` that the lwm2m binary integrates with.
>
> **Status (2026-05-31):** D1 + D2 (PR #29), D3 (PR #30), D4 (PR #31)
> done. D5 + D6 pending.

---

## 0. Goal

Run an OpenVPN tunnel under operator control via `ds-cli`. The
operator sets `vpn.remote.host` / `vpn.remote.port` / `vpn.cert.*` /
etc. in the data store; the new `openvpn-client` daemon picks them
up, spawns `openvpn(8)` with a generated config, watches its
management interface, parses the `PUSH_REPLY` from the server, and
writes the assigned virtual IP + DNS + state back into the data
store. Other apps (and the operator) observe the tunnel state via
`ds-cli get vpn.*` or by registering a `Client::watch`.

Reference: grace-server has a libevent-based implementation of the
same shape. This repo's port replaces libevent with ACE
(`ACE_Reactor`, `ACE_Event_Handler`, `ACE_SOCK_Stream`,
`ACE_Process`) — same primitives the rest of the codebase already
uses.

### Non-goals (first-cut scope)

- **Reconnect / rekey logic.** Quiesce after the first
  `PUSH_REPLY`; FUP-L12-1 covers the lifecycle.
- **OpenVPN3 SDK embedding.** Subprocess + mgmt interface is simpler
  and matches grace-server.
- **Username/password auth, inlined PEM credentials.** Cert + key
  file paths only for v1. FUP-L12-2 for user/pass; FUP-L12-3 for
  inlined PEM.
- **Multiple concurrent tunnels.** One tunnel per `openvpn-client`
  process. Multi-tunnel keyed by name is a future module.
- **systemd integration.** Land in L13 alongside the iot units;
  manual `./openvpn-client` for L12.

---

## 1. Risk register

| ID    | Risk                                                                                          | Mitigation                                                              |
|-------|------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------|
| R1    | `openvpn(8)` is a privileged process (TUN device, route table writes) — running as non-root needs CAP_NET_ADMIN | Document the cap requirement in module README; smoke harness uses `--privileged` podman, packaging follow-up sets `AmbientCapabilities=` in the systemd unit |
| R2    | Mgmt interface is line-oriented but multi-line (e.g. `PUSH_REPLY` spans into END marker); naive line-at-a-time parsing drops payload | Build a small line buffer + state machine in `mgmt_protocol.cpp` keyed on the `>...:` prefix |
| R3    | openvpn subprocess can die for many reasons (config typo, auth fail, network); we need a deterministic teardown so the data store never lies | `ACE_Process_Manager` + `wait()` on SIGCHLD; on exit, write `vpn.state=exited` + `vpn.exit_code=<n>` to the store before our own exit |
| R4    | Config delta (cert path change via ds-cli) must regenerate the openvpn .conf — for v1 we just log "needs restart"; full hot-reload follows the FUP-DS-9 pattern | First-cut: log on change, restart on next process start. FUP-L12-1 makes it live |
| R5    | Mgmt password / hold semantics — by default openvpn waits for `hold release` before connecting if password set; if not set, connects immediately | First-cut: do NOT set `--management-client-pass`; bind to 127.0.0.1 only so DAC + localhost are the perimeter |
| R6    | DsConfig pattern duplicates a lot of plumbing from `apps/src/ds_config.cpp` | Lift the shared pattern into a small helper later (FUP-L12-4); first-cut: copy + adapt is fine |

---

## 2. D-items

### D1 — `vpn.*` schema ✅ (PR #29)

Closed 2026-05-31. Schema lands at
`modules/openvpn-client/schemas/vpn.lua` with 9 read + 7 write keys
+ defaults for the optional ones. cmake install rule drops it at
`/etc/iot/ds-schemas/vpn.lua`. Smoke verified `vpn.remote.port=99999`
rejected with `schema(vpn.remote.port): 99999 above max 65535`.

**Scope.** Add `modules/openvpn-client/schemas/vpn.lua`. ds-server
auto-loads it from `/etc/iot/ds-schemas/` (FUP-DS-6 default dir).
Operators get `SchemaRejected` at set time on type / range mismatches.

Read by the daemon:

| Key                | Type    | Default        | Purpose                                                  |
|--------------------|---------|----------------|----------------------------------------------------------|
| `vpn.remote.host`  | string  | (none)         | Server hostname or IP                                    |
| `vpn.remote.port`  | uint32  | 1194           | Server port (min=1, max=65535)                           |
| `vpn.remote.proto` | string  | `"udp"`        | `"udp"` or `"tcp"` — validated in code, not schema enum  |
| `vpn.cert.path`    | string  | (none)         | Client X.509 cert path                                   |
| `vpn.key.path`     | string  | (none)         | Client X.509 private key path                            |
| `vpn.ca.path`      | string  | (none)         | Server CA path                                           |
| `vpn.cipher`       | string  | `"AES-256-GCM"`| Data-channel cipher                                      |
| `vpn.dev`          | string  | `"tun"`        | `"tun"` or `"tap"`                                       |
| `vpn.mgmt.port`    | uint32  | 7505           | Localhost port for `--management` socket                 |

Written by the daemon:

| Key                     | Type    | Meaning                                                            |
|-------------------------|---------|--------------------------------------------------------------------|
| `vpn.state`             | string  | `disconnected` / `resolving` / `connecting` / `auth` / `wait` / `connected` / `exited` |
| `vpn.assigned.ip`       | string  | Pushed virtual IP                                                  |
| `vpn.assigned.gateway`  | string  | Pushed VPN gateway                                                 |
| `vpn.assigned.netmask`  | string  | Pushed netmask                                                     |
| `vpn.assigned.dns`      | string  | Comma-separated DNS servers (first cut; array follows in FUP)      |
| `vpn.pid`               | uint32  | Live openvpn subprocess pid                                        |
| `vpn.exit_code`         | int32   | Last openvpn exit code (when state==exited)                        |

**Tests.** `modules/openvpn-client/test/schema_test.cpp` validates the
schema parses + the type / range / default expectations hold.

---

### D2 — Module scaffold ✅ (PR #29)

Closed 2026-05-31. Module tree under `modules/openvpn-client/`
mirrors data-store. v0 binary connects to ds-server, dumps every
known vpn.* key via libdatastore_client, exits. Internal lib
(`openvpn_client_lib`) split out so future test targets link the
same code the binary runs. Schema install rule lands at
`/etc/iot/ds-schemas/vpn.lua`. Module README at `docs/design.md`.

**Scope.** Create `modules/openvpn-client/` mirroring data-store's
shape:

```
modules/openvpn-client/
├── CMakeLists.txt              builds openvpn-client binary; links libdatastore_client
├── inc/openvpn_client/
│   └── client.hpp              public-ish API (Client class for tests)
├── src/
│   ├── client.cpp              connect lifecycle
│   ├── ds_bridge.cpp           reads vpn.* from ds-server, writes back
│   ├── ds_bridge.hpp
│   ├── main.cpp                daemon entry, ACE_Reactor loop
│   ├── mgmt_protocol.cpp       line buffer + >STATE:/>PUSH_REPLY: dispatch
│   ├── mgmt_protocol.hpp
│   ├── process.cpp             ACE_Process wrapper around openvpn(8)
│   └── process.hpp
├── schemas/vpn.lua             (from D1)
├── test/                       gtest suite
└── docs/
    └── design.md               module shape + state machine + protocol notes
```

cmake parent (`apps/CMakeLists.txt`) gets an `add_subdirectory` like
the data-store one. Module compiles to a single `openvpn-client`
binary + an internal lib `openvpn_client_lib` so tests link against
the same code.

**Tests.** Build smoke: cmake configures, builds the binary stub, no
runtime behaviour yet (D3+ add it). Stub `main.cpp` prints a banner
and exits.

---

### D3 — DsBridge: read vpn.* from data-store + connect/watch ✅ (PR #30)

Closed 2026-05-31. `src/ds_bridge.{hpp,cpp}` lands with read
accessors + setters per the L12 plan key list, plus
`missing_required()` that returns the 4 required keys absent (host,
cert.path, key.path, ca.path). Listener-thread watch logs
"needs restart" on every change (R4 first-cut policy); FUP-L12-1
will replace with live re-application.

5 unit tests in `test/ds_bridge_test.cpp` cover the don't-need-a-
live-ds-server paths (construct against bad socket, accessors return
nullopt, missing_required shape, setters no-op when disconnected,
on_change accepts nullptr). Build behind
`-DBUILD_OPENVPN_CLIENT_TESTS=ON`. Smoke against a real ds-server
verifies the "ready for D6" handoff message fires only when all 4
required keys are present.

`v0_dump_vpn_keys` refactored to use DsBridge instead of bare
Client::get so the binary now exercises the bridge end-to-end on
every run.

**Scope.** `ds_bridge.{hpp,cpp}` — analog of `apps/src/ds_config.cpp`,
adapted for vpn keys:

- Owns a `data_store::Client` for the daemon lifetime
- Primes a thread-safe snapshot of the readable keys on startup
- Registers a `watch` for the same keys; logs "needs restart" on
  changes (first-cut policy per R4)
- Exposes `set_state(s)` / `set_assigned_ip(s)` / etc. that issue
  `Client::set` on the daemon's ds-server connection
- Optional connect — daemon refuses to start if any required read
  key is missing, rather than spawning openvpn with empty fields

**Tests.** Unit test that constructs a DsBridge against an in-process
ds-server (same harness as `modules/data-store/test/protocol_test.cpp`).
Assert reads + writes round-trip; assert missing required key surfaces
as a clear error.

---

### D4 — Mgmt-protocol parser ✅ (PR #31)

Closed 2026-05-31. `src/mgmt_protocol.{hpp,cpp}` lands as pure
logic — no sockets, no ACE. `Parser::feed(bytes)` accumulates a line
buffer and emits one `Event` per newline; `next()` pops in FIFO.
Event::Kind covers 15 prefix-classified flavours (Banner / State /
PushReply / ByteCount / Log / Hold / Fatal / Echo / Password /
NeedOk / Client / SuccessReply / ErrorReply / EndMarker / DataLine /
Unknown). Comma-split fields populated for the three async events
that use that shape. `split_push_option()` helper for the option-
name-vs-value follow-on parse.

16 table-driven tests in `test/mgmt_protocol_test.cpp` cover:
banner, STATE field layout, ASSIGN_IP IP-in-field-3, PUSH_REPLY
comma split, split_push_option (bare option + key value + multi-
word value + leading-ws), BYTECOUNT, SUCCESS/ERROR, END marker,
multi-event batch feed, partial-line buffering, CRLF tolerance,
empty-line drop, unknown async event, non-prefixed data line, and
a realistic boot-to-CONNECTED stream from a single feed.

Total: 21 openvpn-client tests now (5 DsBridge + 16 parser);
39/39 ds-tests still green.

**Scope.** `mgmt_protocol.{hpp,cpp}` — pure logic, no I/O. Takes raw
bytes, emits events:

```cpp
struct Event {
    enum class Kind { State, PushReply, ByteCount, Log, Hold, Echo, Unknown } kind;
    std::string body;                  // raw payload after the prefix
    std::map<std::string, std::string> kv; // parsed `k=v` pairs for PUSH_REPLY
};
class Parser {
public:
    void feed(std::string_view bytes);  // accumulate bytes from socket
    std::optional<Event> next();        // pop a fully-parsed event
};
```

Parser handles:
- Line accumulation across short reads
- Async event lines starting with `>`
- Multi-line `PUSH_REPLY` (terminator: `END` on its own line — actually
  PUSH_REPLY is a single line containing all `,`-separated options)
- Command responses (status `SUCCESS:` / `ERROR:`) — for v1 we only
  issue `hold release` then read events; the command-response demuxer
  is FUP-L12-5.

**Tests.** Table-driven parser tests: known fixture inputs (captured
from a real openvpn mgmt session) → expected `Event` stream.

---

### D5 — `ACE_Process` wrapper + lifecycle

**Scope.** `process.{hpp,cpp}`:

- `OpenVpnProcess::spawn(GeneratedConfig)` — writes the generated
  `.conf` to a temp path, launches `openvpn` via `ACE_Process` with
  `--config <tmp> --management 127.0.0.1 <port>`
- Captures stdout/stderr to journald (or stderr-pipe → ACE logger)
- `OpenVpnProcess::wait()` — `ACE_Process::wait` returns the exit
  status when the subprocess dies
- Registers with `ACE_Process_Manager` so SIGCHLD doesn't escape
- `OpenVpnProcess::terminate()` — clean shutdown on daemon exit

GeneratedConfig is the string built from `vpn.*` snapshot values:

```
client
dev tun
proto udp
remote <vpn.remote.host> <vpn.remote.port>
cipher <vpn.cipher>
ca <vpn.ca.path>
cert <vpn.cert.path>
key <vpn.key.path>
nobind
verb 3
management 127.0.0.1 <vpn.mgmt.port>
```

**Tests.** Spawn `/bin/sh -c 'echo hello; exit 7'` as a stand-in to
prove the process plumbing without needing openvpn installed.
Verify exit code is captured.

---

### D6 — Main loop: glue D3 + D4 + D5; first PUSH_REPLY → ds-server

**Scope.** `src/client.cpp` + `src/main.cpp`:

1. Parse CLI flags (`ds-sock=PATH`, `--once` for single-cycle mode).
2. Construct `DsBridge`; refuse to start on missing required keys.
3. Build generated config from snapshot.
4. Spawn openvpn; wait for the mgmt socket to be listening (poll
   connect with 100ms retries up to 5s).
5. Register an `ACE_Event_Handler` on the mgmt socket; route through
   `mgmt_protocol::Parser`.
6. On each `Event`:
   - `>STATE:CONNECTING` → `ds.set_state("connecting")`
   - `>STATE:AUTH` → `ds.set_state("auth")`
   - `>STATE:GET_CONFIG` → `ds.set_state("wait")`
   - `>STATE:ASSIGN_IP,…,<ip>` → `ds.set_assigned_ip(ip)`
   - `>STATE:CONNECTED,…` → `ds.set_state("connected")`
   - `>PUSH_REPLY:dhcp-option DNS …,route-gateway …,ifconfig <ip> <mask>,…`
     → parse `k=v`, write `vpn.assigned.{gateway,netmask,dns}`
7. Quiesce. `--once` mode exits after the first PUSH_REPLY; default
   mode keeps the reactor running until SIGINT.
8. On any subprocess death: write `vpn.state=exited`,
   `vpn.exit_code=<n>`, exit ourselves with the same code.

**Tests.** Two parts:
- Unit test that drives `main`-like logic with a fake mgmt server
  feeding canned `>STATE:` / `>PUSH_REPLY:` lines; assert the
  DsBridge writes match.
- Integration smoke harness in `log/L12/openvpn-smoke.sh` that boots
  a podman openvpn-server side-by-side (using
  `kylemanna/openvpn` or similar), runs our daemon against it,
  verifies `ds-cli get vpn.assigned.ip` returns a non-empty value.

---

## 3. Sequencing

1. **D1 + D2** together — schema + scaffold. Module builds (stub
   binary). Schema lives in /etc/iot/ds-schemas/ after cmake install.
2. **D3** — DsBridge. Unit-testable in isolation; first real
   data-store consumer outside the iot binary.
3. **D4** — Parser. Pure logic, table-driven tests.
4. **D5** — ACE_Process wrapper. Smoke against /bin/sh stand-in.
5. **D6** — Glue + smoke. End-to-end against a real openvpn server.

5–6 PRs total, each independently mergeable. Same cadence as L11.

---

## 4. Open questions (track to closure as D-items land)

| Q   | Question                                                                                | Decision path                                                                       |
|-----|------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------|
| Q1  | Mgmt-interface port collision — what if 7505 is already in use?                          | First-cut: fail with descriptive error. Pick-via-getsockname is FUP-L12-6.          |
| Q2  | Generated `.conf` location — `/tmp/<pid>.conf` vs `/run/iot/openvpn-<pid>.conf`?         | `/run/iot/openvpn-<pid>.conf` so it's under the same RuntimeDirectory= as ds-server. |
| Q3  | What happens if `vpn.cert.path` points at a file that doesn't exist?                    | DsBridge stat()s the path on start; refuses if missing. Schema can't validate paths. |
| Q4  | DNS list — single comma-joined value, or N keys (`vpn.assigned.dns.0`, `…dns.1`)?       | Comma-joined for first cut (simplest); array key namespace if a consumer asks.       |
| Q5  | Do we want `vpn.state=reconnecting` even though we don't auto-reconnect? Just the wire? | No. Only emit states the daemon can actually reach. FUP-L12-1 expands when we add reconnect. |

---

## 5. Related docs

- [L11 plan](../L11/plan.md) — same shape, recently closed (packaging).
- [data-store protocol](../../modules/data-store/docs/protocol.md) — what the daemon speaks to ds-server over EMP.
- [data-store client API](../../modules/data-store/docs/client_api.md) — what DsBridge wraps.
- [apps/src/ds_config.cpp](../../apps/src/ds_config.cpp) — DsBridge's template; same pattern adapted for vpn.*.
- [`grace-server` openvpn-client] — external; libevent variant the user has run before.
