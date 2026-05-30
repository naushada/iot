# Data Store — RDD / TDD Plan

Test-driven phase plan for the persistent Lua-backed Unix-socket
data store described in [`data-store-design.md`](data-store-design.md).
Same legend / shape as `lwm2m-rdd.md` for consistency.

| Token                | Meaning                                              |
|----------------------|------------------------------------------------------|
| REQ-DS-NNN           | Functional requirement                               |
| NFR-DS-NNN           | Non-functional requirement                           |
| BUG-DS-NNN           | Defect in current/expected code (must-fix)           |
| DS-D-N               | Binding decision recorded in §5                      |
| DS-Phase Dn          | Implementation phase (sequential, each pcap-gated)   |
| FUP-DS-N             | Closed-out follow-up                                 |

---

## 1. Scope

The data store is a single-host, single-binary key-value service
exposed over an `AF_UNIX` stream socket. Persistence is a Lua chunk
mirrored on every successful write. Notification fan-out is in-process
on the iot reactor thread. No transactions, no TTL, no replication.

Out-of-scope today: cross-host replication, TLS over the unix socket
(filesystem DAC is enough for v1), schema validation, per-key types.

---

## 2. Functional requirements

| ID         | Requirement                                                                                                    | Pri |
|------------|----------------------------------------------------------------------------------------------------------------|:--:|
| REQ-DS-001 | A `set` request with one key MUST persist the value to disk before the response is written.                    | M  |
| REQ-DS-002 | A `set` request with N keys MUST be applied atomically — either all values are persisted or none.              | M  |
| REQ-DS-003 | A `get` request MUST return the most recent value for each requested key. Missing keys return JSON `null`.     | M  |
| REQ-DS-004 | A `register` request MUST subscribe the calling session to value-change notifications for every listed key.    | M  |
| REQ-DS-005 | A `remove` request MUST unregister the calling session from every listed key without affecting other sessions. | M  |
| REQ-DS-006 | A `set` whose new value equals the current value MUST NOT emit a notification.                                 | M  |
| REQ-DS-007 | A change to a watched key MUST emit exactly one `changed` event per watching session, on the reactor thread.   | M  |
| REQ-DS-008 | Notifications MUST carry the previous value (`prev`) and the new value (`v`).                                  | S  |
| REQ-DS-009 | Disconnecting a session MUST silently drop all of its watches and any pending notifications.                   | M  |
| REQ-DS-010 | Every request and response MUST be a single JSON document terminated by `\n`.                                  | M  |
| REQ-DS-011 | The server MUST support concurrent client sessions; the upper bound is the per-process fd budget.              | M  |
| REQ-DS-012 | `set` / `get` / `register` / `remove` MUST accept an array of keys per request (vectorised ops).               | M  |
| REQ-DS-013 | An optional `id` field on the request MUST be echoed back on the response unchanged.                           | S  |
| REQ-DS-014 | Protocol errors (bad JSON, unknown op, missing fields) MUST yield `ok=false` with a single-line `err` string and MUST NOT close the session. | M |
| REQ-DS-015 | On startup, the server MUST load the on-disk Lua chunk into the in-memory map atomically with `luaL_dofile`. Missing file = empty map (not an error). | M |
| REQ-DS-016 | A corrupted on-disk Lua chunk MUST cause the server to log an ERROR and exit with a distinct exit code (3), not start with stale or empty state. | M |
| REQ-DS-017 | In-process callers (bootstrap, DM, app modules) MUST be able to use the store via a `DataStore&` accessor on `App` without going through the unix socket. | S |

---

## 3. Non-functional requirements

| ID          | Requirement                                                                              | Pri |
|-------------|------------------------------------------------------------------------------------------|:--:|
| NFR-DS-001  | Persistent flush latency (set → fsync → ack) ≤ 5 ms p99 on a typical SSD (1 KiB value).  | S  |
| NFR-DS-002  | A `get` p99 ≤ 500 µs for a key present in-memory.                                        | C  |
| NFR-DS-003  | Crash-safe writes — the file on disk is either the pre-set state or the post-set state, never torn. Verified by injecting `kill -9` between write + rename. | M |
| NFR-DS-004  | The socket lives at `0660 root:iot` (or operator-configurable group); group membership is the only access control. | M |
| NFR-DS-005  | The server MUST tolerate slow clients — if a session's send buffer fills, the dropped notification increments a counter and the session stays open. | S |
| NFR-DS-006  | Adding a new client to an in-flight server with 100 active sessions MUST NOT block any other session > 1 ms.  | C |
| NFR-DS-007  | Every requirement above MUST be covered by at least one GoogleTest case named `<Area>_<RequirementID>`.       | M |
| NFR-DS-008  | A pcap-equivalent — a captured client/server transcript saved as `log/L10/ds-smoke.txt` — MUST exist for every phase that emits wire traffic. | M |

---

## 4. Traceability matrix

Maps every Must-priority requirement to: design clause, phase, and
test artifact. Each row is closed when the linked phase merges.

| ID          | Design § | Phase | Test artifact                                          |
|-------------|---------|------|--------------------------------------------------------|
| REQ-DS-001  | §4.2    | D4   | `data_store_test.cpp::DS_REQ_DS_001_set_persists`      |
| REQ-DS-002  | §4.2    | D4   | `DS_REQ_DS_002_multi_key_set_atomic`                   |
| REQ-DS-003  | §3.3    | D3   | `DS_REQ_DS_003_get_returns_latest` + `missing_is_null` |
| REQ-DS-004  | §3.4    | D5   | `DS_REQ_DS_004_register_subscribes`                    |
| REQ-DS-005  | §3.4    | D5   | `DS_REQ_DS_005_remove_unsubscribes_only_caller`        |
| REQ-DS-006  | §6      | D5   | `DS_REQ_DS_006_unchanged_value_no_notify`              |
| REQ-DS-007  | §5      | D5   | `DS_REQ_DS_007_notify_per_watching_session`            |
| REQ-DS-009  | §6      | D5   | `DS_REQ_DS_009_disconnect_drops_watches`               |
| REQ-DS-010  | §3      | D2   | `DS_REQ_DS_010_line_delimited_json`                    |
| REQ-DS-011  | §2      | D1   | `DS_REQ_DS_011_concurrent_sessions`                    |
| REQ-DS-012  | §3.2    | D2   | `DS_REQ_DS_012_vectorised_ops`                         |
| REQ-DS-014  | §7      | D2   | `DS_REQ_DS_014_bad_json_yields_err`                    |
| REQ-DS-015  | §4.3    | D4   | `DS_REQ_DS_015_load_on_startup`                        |
| REQ-DS-016  | §4.3    | D4   | `DS_REQ_DS_016_corrupted_state_exits`                  |
| REQ-DS-017  | §2      | D1   | `DS_REQ_DS_017_in_process_accessor`                    |
| NFR-DS-003  | §4.2    | D4   | smoke: `log/L10/ds-crash-safe.sh` kills mid-write      |
| NFR-DS-004  | §8      | D1   | `DS_NFR_DS_004_socket_mode`                            |

---

## 5. Decisions log

| Date       | ID    | Decision                                                                                                                                                                                                                                                                                                | Rationale                                                                                                                                                                                                  | Affects                |
|------------|-------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|------------------------|
| 2026-05-30 | DS-D1 | **Wire protocol is line-delimited JSON, not a binary frame.** One JSON doc per `\n`-terminated line, both directions.                                                                                                                                                                                  | Implementable in shell, Python, Node, Go without a CoAP parser; matches the existing CLI / cmd file. Latency cost is rounding-error vs the fsync.                                                          | §3                     |
| 2026-05-30 | DS-D2 | **Write-through, no batching, in v1.** Every successful set/remove rewrites the file via temp + rename + fsync.                                                                                                                                                                                       | Crash-safe by construction; meets NFR-DS-001 at expected v1 load; the batch path is one phase away if profiling shows we need it.                                                                          | §4.2, NFR-DS-001       |
| 2026-05-30 | DS-D3 | **Persistence file shape matches `apps/config/**/*.lua`** — top-level `return { data = {...}, schema_version = 1 }`.                                                                                                                                                                                   | Reuses `iot::lua_config` loader (one tweak: walk the `data` sub-table directly instead of `<object>.resources`). Operators inspect/edit the file with the same toolchain.                                  | §4.1                   |
| 2026-05-30 | DS-D4 | **Single reactor thread owns every fd + the store.** No locks. fsync is on the reactor thread.                                                                                                                                                                                                       | Matches UDPAdapter's threading. Reactor jitter from fsync is acceptable in v1 because the binary's other paths (CoAP, registry) tolerate ms-class blips. If they stop tolerating it, move fsync to ACE_Task. | §5                     |
| 2026-05-30 | DS-D5 | **`remove` does NOT emit a `removed` notification.** Only `changed` events fire today.                                                                                                                                                                                                                | Keeps the wire model simple; no consumer has asked for `removed` yet. Reserved in §3.4 so v2 can add it.                                                                                                  | §3.4, §6, Q1           |
| 2026-05-30 | DS-D6 | **Auth is filesystem DAC on the socket path.** Socket created at `0660 root:<group>`; group membership is the gate.                                                                                                                                                                                   | Matches the deployment model — iot binary as a system service, clients as group members. Avoids an in-protocol token scheme that adds key-management cost.                                                 | §8, NFR-DS-004         |

---

## 6. Implementation phases

Each phase is a separate PR onto `feat/data-store`. Risk gate at the
end of each phase: the listed test artifact must be green before the
next phase begins.

| Phase | Goal                                                              | Closes                                                          | Risk gate                                                                                                  |
|-------|-------------------------------------------------------------------|-----------------------------------------------------------------|------------------------------------------------------------------------------------------------------------|
| **D1** | ACE_LSOCK_Acceptor scaffolding; bind unix socket; accept N sessions; no I/O yet (each session sends a `{"ok":true}` welcome and closes). | REQ-DS-011, NFR-DS-004, REQ-DS-017                              | `DS_REQ_DS_011_concurrent_sessions` connects 16 clients in parallel; all receive welcome.                  |
| **D2** | Line-delimited JSON parser + dispatch; `ok=false,err=…` for unknown ops; vectorised arg shape parsed.                                  | REQ-DS-010, REQ-DS-012, REQ-DS-014                              | `DS_REQ_DS_014_bad_json_yields_err` survives malformed input + session stays open.                         |
| **D3** | In-memory `DataStore` with get/set/remove; protocol wires up; no persistence yet.                                                       | REQ-DS-003                                                      | `DS_REQ_DS_003_get_returns_latest` + missing-key case.                                                     |
| **D4** | LuaPersistor: load at startup, write-through on set/remove; corrupted-state exit.                                                       | REQ-DS-001, REQ-DS-002, REQ-DS-015, REQ-DS-016, NFR-DS-001/003 | `DS_REQ_DS_001_set_persists` + manual `log/L10/ds-crash-safe.sh` (kill -9 mid-write, restart, verify state). |
| **D5** | Notification engine: register / remove / fan-out / unchanged-skip; per-session subscription set.                                        | REQ-DS-004..009                                                 | `DS_REQ_DS_007_notify_per_watching_session` covers fan-out across 3 sessions watching the same key.        |
| **D6** | `ds-cli` test client + `log/L10/run-ds-smoke.sh` heredoc-driven smoke harness mirroring `log/L9/run-cli-smoke.sh`.                       | NFR-DS-008                                                      | Smoke captures the 6-step session in §3.5 of the design doc.                                               |
| **D7** | Docs: update `architecture.md`, add `Related docs` cross-links, mark RDD rows closed, write `log/L10/results.md` mirroring L9 style.    | —                                                              | Doc-only; no test.                                                                                         |

Test rules per phase (mirrored from `lwm2m-rdd.md` §6):

1. **TDD shape** — every new file ships with at least one GoogleTest
   case named `DS_<RequirementID>` covering the change.
2. **No skip on the closure side** — the risk-gate test must pass; if
   it's flaky, fix the flake before closing the phase.
3. **In-tree wire evidence** — `log/L10/` carries `.txt` transcripts
   (no pcap; the unix socket has no on-the-wire format to capture).
4. **No new BUG-DS-IDs without a follow-up** — defects found during
   a phase either get fixed in the phase PR or filed as their own
   `BUG-DS-NNN` row with an open FUP-DS.

---

## 7. Risk register

| ID         | Risk                                                                                                                | Likelihood | Impact | Mitigation                                                                                            |
|------------|---------------------------------------------------------------------------------------------------------------------|:----------:|:------:|-------------------------------------------------------------------------------------------------------|
| RISK-DS-01 | fsync on reactor thread spikes CoAP / registry latency under load.                                                  | M          | M      | Profile in D4; if it shows, move flush to ACE_Task worker on D5.                                      |
| RISK-DS-02 | A misbehaving client floods set requests, blocking the reactor on consecutive fsync calls.                          | M          | H      | Per-session rate cap (10 set/s) in D5; configurable via `ds-set-rate=`.                               |
| RISK-DS-03 | The persistence file grows unbounded with one-shot keys (e.g. session tokens never `remove`d).                      | L          | M      | Document the pattern: callers MUST `remove` ephemeral keys. Optional TTL is a v2 ask.                 |
| RISK-DS-04 | `rename(2)` is NOT atomic across filesystem boundaries (e.g. `/var/lib/iot` on a different FS than `/var/lib/iot/data_store.lua.tmp`). | L          | H      | D4 abort if `/var/lib/iot` isn't on a single FS at startup; document the assumption.                  |
| RISK-DS-05 | Recovery from a corrupted file is operator-only (we exit 3) — easy to deploy into a restart loop.                   | L          | M      | systemd unit caps restart attempts; doc the manual recovery procedure (move file aside, restart).     |

---

## 8. Test-suite map

Test files land under `apps/test/` and link the same `liblwm2m_test`
target as the existing suite. Mock surfaces:

- `LuaPersistor` accepts an injectable `Filesystem` (default = real
  `open/write/fsync/rename`) so unit tests can drive the failure
  modes without touching disk.
- `DataStoreSession` accepts an injectable `WriteSink` (default =
  the ACE socket) so the notification fan-out can be unit-tested
  without spinning up a real listener.

Required test files (one per phase that produces one):

- `data_store_test.cpp` — all DS_REQ_DS_* unit tests.
- `lua_persistor_test.cpp` — D4-specific persistence corner cases.
- `data_store_session_test.cpp` — D2/D5 protocol + notification cases.

---

## 9. Related docs

- [`data-store-design.md`](data-store-design.md) — the design half.
- [`lwm2m-rdd.md`](lwm2m-rdd.md) — RDD format this doc mirrors.
- [`architecture.md`](architecture.md) — will gain a "DataStore"
  box in §X once D1 lands.
