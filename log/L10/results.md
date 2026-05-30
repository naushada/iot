# L10 Data-Store Results

End-of-phase closure record for the `modules/data-store/` feature
branch (`feat/data-store`). Mirrors `log/L9/results.md` shape — one
section per closed phase, with risk-gate evidence, tests run, and
on-disk artefacts.

## Status: D1, D1.5, D2, D3, D4, D5, D6, D7 — **all closed**.

22/22 unit tests green; two end-to-end smokes captured.

---

## D1 — Module scaffold + LSOCK acceptor + welcome round-trip

**Closed 2026-05-30** (commit `650120e`).

Brought up the `modules/data-store/` tree as a standalone build:
- `libdatastore_client.a` — POSIX-clean public header (pimpl over
  ACE internally), `Client::connect` / `recv_welcome`.
- `ds-server` — `ACE_LSOCK_Acceptor`, bind at `/var/run/iot/data_store.sock`
  with mode `0660`, single-shot welcome line per accepted connection.
- `ds-cli` — connect, print welcome, exit.
- `ds-tests` — gtest-driven suite.

The cmake invocation produces all four targets from a single source
tree, with no edits required in `apps/CMakeLists.txt` (REQ-DS-018).

### Risk gates closed

| Req | Test | Evidence |
|-----|------|----------|
| REQ-DS-011 | `ServerIntegration.DS_REQ_DS_011_concurrent_sessions` | 16 parallel `Client::connect` + `recv_welcome` round-trips |
| NFR-DS-004 | `ServerIntegration.DS_NFR_DS_004_socket_mode_is_0660` | `stat(socket).st_mode & 0777 == 0660` |
| REQ-DS-018 | manual `cmake .. && make -j2` | produces all 4 targets |
| REQ-DS-019 | inspection of `inc/data_store/client.hpp` | only `<cstdint>`/`<memory>`/`<string>` |

---

## D1.5 — Active-object worker pool (xpmile MicroService shape)

**Closed 2026-05-30** (commit `6d4c37c`; client lib POSIX→ACE swap in
`8800b2f`).

Per project memory + grace-server reference: each accepted connection
hands off to one of 5 `Worker` (`ACE_Task<ACE_MT_SYNCH>`) workers via
round-robin. Workers consume `ACE_Message_Block`s carrying a
`WorkRequest*` from their `msg_queue`. Reactor thread is never
blocked by per-request work.

### Risk gates closed

| Test | What it proves |
|------|----------------|
| `WorkerPool.default_size_is_five` | pool default == 5 |
| `WorkerPool.round_robin_visits_every_worker_then_wraps` | 6 picks across 3-pool: indices 0,1,2,0,1,2 |
| `WorkerPool.next_before_open_returns_nullptr_when_pool_size_zero` | zero-size guard |

---

## D2 + D3 + D5 — Full set/get/register/remove + notify fan-out

**Closed 2026-05-30** (commit `51c92f3` — landed as a single PR per
operator preference; the three phases interlock too tightly to split
cleanly).

Wire protocol per design §3: line-delimited JSON, one document per
`\n`, vectorised key arrays, optional `id` correlation, `{ok,
err}` error model. Parsed via vendored `nlohmann::json` (already at
`apps/3rdparty/json/`).

- **Session** (`session.{hpp,cpp}`): long-lived per-connection state.
  Reactor thread reads; reads enqueue `ProcessRequest` to the
  session's owner Worker. Worker writes responses + notify
  deliveries.
- **DataStore** (`data_store.{hpp,cpp}`): thread-safe via mutex.
  `set()` returns `SetResult{changed, prev, watchers}` so the
  dispatching Worker can fan out notifications without holding the
  lock across socket writes.
- **Fan-out**: same-Worker watchers get a direct `send()`;
  cross-Worker watchers receive a `DeliverNotify` message on their
  owner Worker's queue (same shape as xpmile MicroService
  cross-thread dispatch). Single-reader / single-writer per socket
  means no socket-level locking.

### Risk gates closed

| Req | Test | What it proves |
|-----|------|----------------|
| REQ-DS-003 | `Protocol.DS_REQ_DS_003_set_then_get_returns_latest` | vector get, missing key → null |
| REQ-DS-006 | `Protocol.DS_REQ_DS_006_unchanged_value_no_notify` | watcher receives no event when set rewrites with the same value |
| REQ-DS-007 | `Protocol.DS_REQ_DS_007_register_then_set_emits_notify_to_watcher` | two clients across the socket; watcher receives `ev=changed` with `prev` |
| REQ-DS-006 (unit) | `DataStore.unchanged_set_reports_not_changed` | `SetResult.changed=false` on rewrite |

### Test harness note

Protocol tests run in-process against a real `Server + WorkerPool` +
real `Client` connections. The `pump_until(futures...)` helper drives
the reactor in 20 ms slices until every async client future is ready
(or 8 s hard cap) — a fixed `pump(N)` count starved the async
clients when their wall-clock work exceeded it.

---

## D4 — Lua persistor (load on boot, write-through on set/remove)

**Closed 2026-05-30** (commit `f1fda73`).

`LuaPersistor` reads + writes the on-disk store at the configured
`ds-store=` path. Shape per design §4.1:

```lua
return {
  schema_version = 1,
  data = {
    ["counter"] = "42",
    ["foo"]     = "bar",
  },
}
```

- **Load**: `luaL_dofile` at startup. Missing file → empty map (not
  an error). Parse failure / missing `data` table → `CorruptStoreError`
  → daemon exits with code 3 (REQ-DS-016), refusing to start with
  silent stale state.
- **Save**: write to `<path>.tmp` → `fsync` → `rename(2)` — crash-safe
  by construction; the file is either pre-set or post-set state on
  disk, never torn (NFR-DS-003).
- **Hook**: `DataStore::set/remove` copy a snapshot under the mutex,
  release the mutex, then call `persistor->save(snapshot)` so other
  workers can proceed during fsync. Unchanged-value sets short-circuit
  before the flush (REQ-DS-006 + persist cost saved).

### Risk gates closed

| Req | Test / evidence |
|-----|-----------------|
| REQ-DS-001 | `DataStorePersist.set_flushes_to_disk_and_load_restores` |
| REQ-DS-015 | `LuaPersistor.load_returns_empty_when_file_missing` |
| REQ-DS-016 | `LuaPersistor.corrupted_file_throws_CorruptStoreError` + `missing_data_table_throws` |
| NFR-DS-003 | `log/L10/run-ds-persist-smoke.sh` — round 1 sets foo+counter, server killed, round 2 loads from disk and serves the same values |

### Wire evidence

`log/L10/ds-persist-smoke.txt` — full transcript:

```
----- on-disk Lua chunk (host-side) -----
return {
  schema_version = 1,
  data = {
    ["counter"] = "42",
    ["foo"]     = "bar",
  },
}
[persist] killing ds-server (simulated crash)
[persist] starting ds-server (round 2 — should reload state)
----- ds-cli get foo counter missing -----
foo=bar
counter=42
missing=(null)
```

---

## D6 — Extended ds-cli + end-to-end smoke harness

**Closed 2026-05-30** (commit `33d54e1`).

`ds-cli` grew real subcommands:

```
ds-cli [--socket=PATH] [welcome]
ds-cli [--socket=PATH] set <key> <value>
ds-cli [--socket=PATH] get <key> [<key>...]
ds-cli [--socket=PATH] watch [--count=N] <key> [<key>...]
ds-cli [--socket=PATH] unwatch <key> [<key>...]
```

`log/L10/run-ds-smoke.sh` spins `ds-server` in podman and drives it
through `podman exec`-ed `ds-cli` calls; the host-visible transcript
demonstrates every D2/D3/D5 path on the wire.

### Risk gate closed

`log/L10/ds-smoke.txt`:

```
{"ok":true,"hello":"data-store-server","v":1}
[set foo bar] ok
[get foo missing] foo=bar / missing=(null)
[watcher] watching 1 key(s); count=1
[set foo baz] ok
[event] foo = baz  (prev=bar)
[unwatch foo] ok
```

---

## D7 — Docs sweep (this section + TDD closure markers + architecture)

**Closed 2026-05-30** (this commit).

- `modules/data-store/docs/tdd.md` §6 — every phase row marked closed
  with the date and originating commit hash.
- `apps/docs/architecture.md` — new "Data-store module" subsection
  pointing at `modules/data-store/`.
- `log/L10/results.md` — this file.

---

## Test summary

```
[==========] 22 tests from 7 test suites ran. (689 ms total)
[  PASSED  ] 22 tests.
```

Breakdown:
- `Proto.*` (3) — op parse round-trip, welcome line shape
- `DataStore.*` (5) — get/set/remove + unchanged-set
- `DataStorePersist.*` (1) — set → flush → restart → restore
- `LuaPersistor.*` (4) — load, save+load, corrupted, missing-table
- `WorkerPool.*` (3) — size, round-robin, zero-size guard
- `ServerIntegration.*` (3) — welcome, 16-concurrent-sessions, socket mode
- `Protocol.*` (3) — set+get, register+notify, unchanged-no-notify

## Open follow-ups (next-PR fodder)

| ID         | Item                                                                                                                              |
|------------|-----------------------------------------------------------------------------------------------------------------------------------|
| FUP-DS-1   | Profile fsync cost under realistic load. If reactor-thread jitter shows up in CoAP latency (per RISK-DS-01), move flush to an ACE_Task worker. |
| FUP-DS-2   | Per-session rate cap (`ds-set-rate=`) to mitigate RISK-DS-02 (a single client flooding sets).                                       |
| FUP-DS-3   | Q1 (`remove`-implies-notify) reopened only if a consumer asks; currently no.                                                       |
| FUP-DS-4   | Q2 (per-key max value size) — pick a cap when the first real consumer lands; today there is no enforced cap.                       |
