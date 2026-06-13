# Local Data Store — Design

> **Status (2026-05-31).** This document captures the original design
> intent from the D1–D10 phases. Several parts have been **superseded
> by later phases**; each affected section flags what changed and
> points at the current source-of-truth doc. The high-level
> architecture (LSOCK acceptor → SessionRegistry → WorkerPool →
> DataStore + LuaPersistor) is unchanged; the wire framing, typing,
> and threading details below were upgraded in PR #5 (typed values),
> PR #7 (EMP framing), PR #11 (schema defaults), PR #12 (live watch).
>
> If you only need the *current* behaviour, read these instead:
>
> - [protocol.md](protocol.md) — current wire (EMP framing + typed values)
> - [client_api.md](client_api.md) — application-developer guide
> - This file — design rationale + historical context


A persistent key-value store delivered as a **separate module** under
`modules/data-store/`, built into three deliverables:

| Target                          | Kind          | Audience                                  |
|---------------------------------|---------------|-------------------------------------------|
| `libdatastore_client.a`         | static lib    | Any C++ app that wants store access       |
| `ds-server`                     | binary        | Standalone daemon (systemd service)       |
| `ds-cli`                        | binary        | Operator debugging client                 |

The iot binary, when it wants store access, links
`libdatastore_client.a` and talks to a running `ds-server` over the
unix socket — same protocol, same boundary, no in-process shortcut.

Backed by a Lua chunk on disk (same shape as `apps/config/**/*.lua`)
so operators can inspect and hand-edit the state with the same
toolchain.

This doc is the design half of the deliverable; the
phase / requirement plan lives in [`tdd.md`](tdd.md).

---

## 1. Goals & non-goals

### Goals
1. **Key-value with notifications.** Clients can `set`, `get`,
   `remove` keys, and `register` for value-change notifications on
   one or more keys.
2. **Persistent.** Survives daemon restart with no data loss for
   committed sets. Recovery is a single `luaL_dofile` of the on-disk
   chunk.
3. **Single-host, separate-binary.** `ds-server` is a standalone
   daemon. No network exposure; auth is unix-socket DAC.
4. **Reactor-driven.** The daemon owns an ACE_Reactor on the main
   thread; every fd lives on that thread so no locking inside the
   server. Clients are independent processes.
5. **Multi-client.** N concurrent client sessions; one notify push
   per registered key per session.
6. **Vectorised ops.** A single request can carry an array of keys
   so a client doesn't pay N round-trips to set/get/register N
   values.
7. **Light client lib.** `libdatastore_client.a` ships with no ACE
   dependency — POSIX sockets + std::string only — so it can drop
   into any C++17 app without inheriting the daemon's toolchain.

### Non-goals (v1)
- Transactions, conditional writes, CAS.
- TTL / expiry on keys.
- Range queries / prefix scans.
- Cross-host replication.
- Schemas, types, or validation — values are opaque strings.
- Wire compression (the unix socket is already in-kernel; no benefit).

---

## 2. Architecture

```
   ┌──────────────┐    ┌──────────────┐    ┌──────────────┐
   │ app process  │    │ iot binary   │    │ ds-cli       │
   │ (links       │    │ (optionally  │    │              │
   │ libdatastore_│    │ links the    │    │              │
   │ client.a)    │    │ same lib)    │    │              │
   └──────┬───────┘    └──────┬───────┘    └──────┬───────┘
          │ AF_UNIX           │ AF_UNIX           │ AF_UNIX
          │ /var/run/iot/data_store.sock          │
          └──────────────┬────┴───────────────────┘
                         ▼
                ┌──────────────────────────────────┐
                │  ds-server  (separate binary)    │
                │  ┌────────────────────────────┐  │
                │  │ Server (ACE_LSOCK_Acceptor)│  │
                │  └────────────────────────────┘  │
                │             │                    │
                │             ▼ accept             │
                │  ┌────────────────────────────┐  │
                │  │ Session ×N (ACE_LSOCK_     │  │
                │  │ Stream + sub set)          │  │
                │  └────────────────────────────┘  │
                │             │                    │
                │             ▼                    │
                │  ┌────────────────────────────┐  │
                │  │ DataStore (in-mem map)     │  │
                │  └────────────────────────────┘  │
                │             │ on change          │
                │             ▼                    │
                │  ┌────────────────────────────┐  │
                │  │ LuaPersistor               │  │
                │  └────────────────────────────┘  │
                │             │ atomic write       │
                │             ▼                    │
                │  /var/lib/iot/data_store.lua     │
                └──────────────────────────────────┘
```

Layer map across the repo:

| Layer                          | Location                                                       |
|--------------------------------|----------------------------------------------------------------|
| Public client API              | `modules/data-store/inc/data_store/client.hpp`                 |
| Client lib impl                | `modules/data-store/src/client/`                               |
| Shared wire-protocol constants | `modules/data-store/inc/data_store/proto.hpp` + `src/proto/`   |
| Daemon (`ds-server`)           | `modules/data-store/src/server/{main,server,data_store}.cpp`   |
| CLI debug client (`ds-cli`)    | `modules/data-store/src/cli/ds_cli.cpp`                        |

Build: `cd modules/data-store && mkdir build && cd build && cmake .. && make`.
Top-level `apps/CMakeLists.txt` is untouched — the iot binary
doesn't currently depend on the data store; when it does, it'll
`target_link_libraries(lwm2m datastore_client)` and use the public
header.

---

## 3. Wire protocol

> **⚠ Superseded (PR #7).** §3.1–§3.5 below describe the original
> newline-delimited JSON wire. The shipping wire is **Embedded Micro
> Protocol (EMP)** — fixed 8-byte big-endian header + JSON payload —
> modelled on Eclipse Mihini's protocol. Read [protocol.md](protocol.md)
> for the authoritative spec. The section is kept here so the historical
> rationale (single-line, language-portable, no CoAP parser) stays
> discoverable.

Newline-delimited JSON over the stream socket. One JSON document per
line, both directions. The client framing is `[\n]` — easy to
implement in any language without a CoAP parser.

### 3.1 Request shape

```json
{ "op": "<set|get|register|remove>", "keys": [ ... ], "id": <opt> }
```

- `op` (required, string) — one of `set`, `get`, `register`, `remove`.
- `keys` (required, array) — see per-op shape below.
- `id` (optional, integer) — opaque client-correlation tag echoed in
  the response. Lets a client multiplex many in-flight requests on
  one connection. Defaults to omitted; if omitted, the response omits
  it too.

### 3.2 Per-op key shapes

| op         | `keys` element                              | Vector? |
|------------|---------------------------------------------|---------|
| `set`      | `{"k":"<name>","v":"<value>"}`              | yes     |
| `get`      | `"<name>"` (bare string)                    | yes     |
| `register` | `"<name>"` (bare string)                    | yes     |
| `remove`   | `"<name>"` (bare string)                    | yes     |

A `set` request:

```json
{"op":"set","keys":[{"k":"foo","v":"bar"},{"k":"baz","v":"42"}],"id":17}
```

A `get` request:

```json
{"op":"get","keys":["foo","baz"],"id":18}
```

### 3.3 Response shape

```json
{ "ok": <true|false>, "id": <opt>, "data": [ ... ], "err": "<opt>" }
```

- `ok` (required, bool) — request succeeded as a whole.
- `id` (optional, mirrored from request).
- `data` (only on `get` and only if `ok=true`) — array of
  `{"k":"<name>","v":"<value>"}` in the same order as the request
  keys. A missing key has `v` set to `null` (not the string `"null"`).
- `err` (optional, string) — present iff `ok=false`. Single-line
  free-text reason.

A `get` response:

```json
{"ok":true,"id":18,"data":[{"k":"foo","v":"bar"},{"k":"baz","v":"42"}]}
```

A failed request:

```json
{"ok":false,"id":19,"err":"unknown op 'fetch'"}
```

### 3.4 Notification push

When a `set` (from any session) changes a value for a key that has
at least one registered watcher, the server pushes one notification
per watching session:

```json
{ "ev": "changed", "k": "<name>", "v": "<new>", "prev": "<old-or-null>" }
```

- `ev` (required) — currently only `"changed"`. `"removed"` reserved
  for v2 when remove triggers a notification.
- No `id` field — push notifications are not correlated with any
  prior request id.

Push delivery is best-effort: if the session's send buffer is full,
the notification is dropped and a `dropped` counter increments on
the server stats path. Clients that need at-least-once semantics
should re-`get` after reconnecting.

### 3.5 Sample session

```
C → {"op":"set","keys":[{"k":"foo","v":"bar"}],"id":1}
S → {"ok":true,"id":1}
C → {"op":"register","keys":["foo"],"id":2}
S → {"ok":true,"id":2}
C → {"op":"get","keys":["foo","missing"],"id":3}
S → {"ok":true,"id":3,"data":[{"k":"foo","v":"bar"},{"k":"missing","v":null}]}
   (some other session does set foo=quux)
S → {"ev":"changed","k":"foo","v":"quux","prev":"bar"}
C → {"op":"remove","keys":["foo"],"id":4}
S → {"ok":true,"id":4}
```

---

## 4. Persistence

### 4.1 On-disk shape

```lua
-- /var/lib/iot/data_store.lua
return {
  schema_version = 1,
  data = {
    foo = "bar",
    counter = "42",
    ["lwm2m.observe.attrs"] = "<json-blob>",
    -- ...
  },
}
```

Same shape as `apps/config/**/*.lua` so the existing `iot::lua_config`
loader can read it (one new key: `schema_version`, gated on the
backing-table iteration).

### 4.2 Write policy

Write-through with atomic rename:

1. Serialize current in-memory map to a string of Lua source.
2. `open(path + ".tmp", O_WRONLY|O_CREAT|O_TRUNC)` → write → `fsync`.
3. `rename(path + ".tmp", path)`.

`rename(2)` is atomic on POSIX local filesystems; either the old or
the new file is visible at any moment, never a torn write. The cost
is one rewrite per `set`/`remove` — acceptable for the expected
load (<= ~10 writes / second). If a deployment outgrows that, the
next phase batches writes onto a 100 ms tick.

### 4.3 Load policy

`DataStore::open(path)` runs once at startup:

1. If `path` does not exist → start with an empty map. Not an error.
2. If it exists but `luaL_dofile` returns non-OK → log + abort
   (corrupted state is worse than restart loop; operator must
   intervene). Exit code distinguishes this from other failures.
3. If the top-level table has no `schema_version` → treat as v0 and
   migrate (v1 introduces no breaking changes; v0 loaders are
   forward-compatible).

### 4.4 Host paths & reboot durability

The store path is set per deployment via `ds-store=` (device/Yocto) or
`--persist-dir=` (cloud, file is `<dir>/data_store.lua`). It is always
**`/var/lib/iot/data_store.lua`** *inside the daemon's namespace*; where that
lands on the host:

| Deployment | Host path |
|---|---|
| Yocto / real RPi | `/var/lib/iot/data_store.lua` on the SD-card rootfs (the device *is* the host; `iot.conf` `systemd-tmpfiles`-creates `/var/lib/iot` 0750 on first boot) |
| Cloud VM (podman/docker) | `cloud_iot-lib` named volume → `…/storage/volumes/cloud_iot-lib/_data/data_store.lua` |
| Device dev (podman) | `device_dev-lib` named volume → `…/storage/volumes/device_dev-lib/_data/data_store.lua` |

Resolve a volume's host path with
`<runtime> volume inspect <name> --format '{{.Mountpoint}}'`. On macOS the
volume lives inside the podman VM (reach it via `podman machine ssh`, or read
through the container: `<runtime> exec iot-ds-server cat /var/lib/iot/data_store.lua`).

**Reboot durability: yes, contents persist.** Every `set`/`remove` is
write-through + `fsync` (§4.2, REQ-DS-001), so committed values are on
persistent storage before the ack — not just in memory. Across a host reboot:
- **Yocto/RPi:** `/var/lib/iot` is on the writable rootfs partition → survives.
- **Cloud / dev (containers):** the data lives in a **named volume**, which
  survives container restart *and* host reboot. It is destroyed only by an
  explicit `<runtime> volume rm <name>`.

Caveats:
- `RESET_CONFIG=1` in `run.sh` removes the **config/schema** volume
  (`cloud_iot-etc` / `device_dev-etc`) to reload image-provided schemas — it
  does **not** touch the **data** volume (`iot-lib` / `dev-lib`), so the store
  survives a `RESET_CONFIG` start.
- Atomic `rename(2)` requires the `.tmp` and the final file to be on one
  filesystem (RISK-DS-04) — they are, both under `/var/lib/iot`.
- The file grows with one-shot keys unless callers `remove` ephemeral entries
  (RISK-DS-03).
- A hard power-cut *during* the rename can leave the pre-`set` file (never a
  torn write); a clean reboot is always safe.

---

## 5. Threading

> **⚠ Superseded (D1.5 / commit `6d4c37c`).** The single-reactor-thread
> model below was replaced by an active-object **WorkerPool** of 5
> `ACE_Task` workers (xpmile MicroService shape). The reactor thread
> only accepts + reads bytes; per-session work runs on the owning
> Worker. `DataStore` mutex-guards the map; `LuaPersistor::save` is
> called under the Worker thread, not the reactor. Cross-Worker notify
> fan-out goes through a `DeliverNotify` WorkMsg on the owner Worker's
> queue so the per-session "single writer to its socket" invariant
> still holds. The fsync analysis at the end is unchanged — flush still
> happens synchronously on the Worker thread; FUP-DS-1 tracks moving
> it to a dedicated worker if profiling demands it.

All sockets, the parser, and the persistor live on the **iot reactor
thread** (same `ACE_Reactor::instance()` used by UDPAdapter). No
locks anywhere. Push notifications are emitted inline from the
`set`-handling callback, on the reactor thread, so ordering matches
the order of `set`s.

The blocking part is the `fsync` in the write-through flush.
Measurement budget says this is ~1 ms on a typical SSD; if profiling
ever shows it eating into reactor latency, the flush moves onto a
dedicated `ACE_Task` worker thread with a queue. Not done in v1.

---

## 6. Key-watch semantics

- One `register` per (session, key) pair. A duplicate `register` for
  a key already watched by that session is a no-op (returns ok).
- A session can watch any number of keys; the upper bound is whatever
  the per-process fd / memory budget allows.
- `remove` removes only the calling session's watch. Other sessions
  watching the same key keep theirs.
- When a session disconnects, all its watches are silently dropped.
- A `set` to a key whose value is unchanged still completes
  successfully but does NOT emit a notification. Comparison is
  byte-equal on the raw value string.

---

## 7. Error model

| Class            | Examples                                  | Response                                |
|------------------|-------------------------------------------|-----------------------------------------|
| Protocol error   | Malformed JSON, unknown op, missing keys  | `ok=false`, descriptive `err`, session stays open |
| Per-key error    | (no v1 cases — all keys accepted)         | n/a                                     |
| Disk error       | Persistor write fails                     | `ok=false`, `err="persist failed: <strerror>"`. In-memory map IS still updated — clients see the new value on `get` but it's lost on restart. Logged at ERROR. |
| Server overload  | accept(2) drops a new connection           | Client sees ECONNREFUSED; no JSON response possible |

The session is never closed by the server in response to a single
bad request — the client decides when to reconnect.

---

## 8. Security

- Socket lives under `/var/run/iot/` with mode `0660` and group
  `iot`. Linux DAC controls access; no in-protocol auth.
- A future v2 might layer an HMAC or token check on each request,
  for shared-host deployments where the iot binary runs as root but
  app clients run as a less-privileged user.

---

## 9. Configuration surface

> **Updated (PR #11).** ds-server is a separate daemon — there is no
> in-process `ds-enable=` toggle. The current CLI surface is:

| Flag              | Default                            | Purpose                                                                                       |
|-------------------|------------------------------------|-----------------------------------------------------------------------------------------------|
| `ds-socket=`      | `/var/run/iot/data_store.sock`     | Listen path; created with mode `0660`.                                                       |
| `ds-store=`       | `/var/lib/iot/data_store.lua`      | Persistent backing file (typed values, `schema_version=2`).                                  |
| `ds-schema-dir=`  | `/etc/iot/ds-schemas/`             | Loads every `*.lua` schema under this dir on boot. Missing dir → silently treated as no schemas. |

The iot client (lwm2m binary) selects its socket via `ds-sock=PATH`
on its own CLI; see `apps/src/main.cpp` + `apps/src/ds_config.cpp`.

---

## 10. CLI test client

> **Updated (PR #7, PR #8).** ds-cli now lives at
> `modules/data-store/src/cli/ds_cli.cpp` and is shipped as the
> separate `ds-cli` target. It's argv-driven (one command per invocation),
> not REPL-driven. Values are parsed JSON-first so types round-trip
> correctly. See [protocol.md §9](protocol.md#9-ds-cli-examples) for the
> current example set.

```sh
$ ds-cli --socket=/var/run/iot/data_store.sock set iot.endpoint '"urn:dev:client-7"'
ok
$ ds-cli --socket=/var/run/iot/data_store.sock get iot.endpoint
iot.endpoint=urn:dev:client-7
$ ds-cli --socket=/var/run/iot/data_store.sock watch iot.endpoint
watching 1 key(s) via callback handle=1; count=1
[event] iot.endpoint = urn:dev:client-9  (prev=urn:dev:client-7)
```

The CLI is a smoke-harness convenience; library integrations link
`libdatastore_client.a` instead of shelling out — see
[client_api.md](client_api.md).

---

## 11. Open questions (track to closure)

| Q   | Question                                          | Resolution path                           |
|-----|---------------------------------------------------|-------------------------------------------|
| Q1  | Should `remove` (key un-register) imply a final notify-of-removal? | Default no per §6. Re-open if a client team asks. |
| Q2  | Per-key max value size?                            | Pick after the first real consumer lands; default cap 64 KiB until then. |
| Q3  | ~~In-process accessor for bootstrap / DM?~~        | **Closed: no.** Architecture pivoted to a separate-binary model. The iot binary talks to `ds-server` via the same unix-socket path as any other client; no in-process shortcut. |

---

## 13. Client threading model (D8 — callback API)

Implemented 2026-05-31 (commits `1fb4909` / `25a4e4b`); reframed onto
EMP framing in PR #7.

`Client::connect()` spawns an **internal listener thread** that owns
the read side of the socket. Every complete EMP frame (one 8-byte
header + payload) is demuxed by `type` byte into one of three lanes:

| Frame type byte                    | Destination                                                                                           |
|------------------------------------|-------------------------------------------------------------------------------------------------------|
| `Push` (bit 1) `NotifyEvent`       | Pull queue (`recv_event()`) **and** every matching per-watch callback                                |
| `Response` (bit 0)                 | Per-request `std::promise<PendingValue>` matched by `(op << 8 \| reqID)` → caller's `set/get/...` returns |
| (anything else)                    | Dropped silently — server only ever sends responses or pushes                                         |

EMP has no welcome handshake; the welcome promise + `recv_welcome()`
were removed in PR #7.

Synchronous methods (`set`, `get`, `watch`, `unwatch`) keep their
sync signatures from the caller's view: they build a request, register
a promise in the pending-map, send the JSON line, wait on
`std::future::wait_for(timeout)`. The listener fulfils the promise on
response arrival. Result: the same Client is safe to drive from any
thread for sync requests while events arrive on the listener.

### Per-watch callback registry

`watch(keys, EventCallback cb)` returns a `WatchHandle`. Multiple
`watch()` calls — same Client, different key sets, different
callbacks — all stay live. On `ev=changed` the listener iterates the
watch table and invokes every callback whose key set contains the
event's key.

A per-key **local refcount** keeps the wire-level `register` /
`remove` tidy:

- First local watcher for a key → send `register` on the wire.
- Subsequent local watchers for the same key → no wire traffic.
- `unwatch(handle)` drops the handle's keys; only when a key's
  refcount hits zero does `remove` go out on the wire.

So three independent callbacks all watching `"foo"` produce exactly
one `register {foo}` round-trip; the third `unwatch` produces the
matching `remove`.

### Pull queue still works

Apps that prefer a poll-style loop call the no-callback `watch(keys)`
overload and read from `recv_event(timeout_ms)`. The listener still
fans events into the queue regardless of whether any callbacks are
registered, so pull + callback styles can mix on one Client.

---

## 14. Per-client Lua schemas (D9)

Implemented 2026-05-31 (commit `25a4e4b`).

`ds-server` accepts an optional `ds-schema-dir=PATH` CLI flag. At
startup every `*.lua` under that directory is loaded into a single
`SchemaRegistry` (immutable after load — no locks on the read path).
Each client/user owns one file:

```lua
-- /etc/ds-schemas/iot.lua
return {
  namespace = "iot",
  keys = {
    ["iot.lifetime"]  = { type="integer", default=86400, min=0 },
    ["iot.endpoint"]  = { type="string",  default="urn:dev:client-1" },
    ["iot.identity"]  = { type="opaque" },
  },
}
```

The `namespace` field is informational; gating is by the full key
name across every loaded file. Duplicate keys log a WARNING via
ACE — last-loaded wins, but operators see the collision.

### Validation hooks in Worker dispatch

- **set**: each `{key: value}` entry runs `schema.validate_set(k, v)`
  against the *typed* `Value` variant (PR #5). Mismatch → EMP response
  with `Status::SchemaRejected` (`0x8004`) and
  `body = {"err":"schema(K): expected …, got …"}`. The request bails
  before touching the store, so a malformed set has zero side effect
  on memory or disk.
- **get**: when `DataStore::get(k)` returns `nullopt`, the worker
  falls back to `schema.default_for(k)` — which now returns a typed
  `Value` (PR #5), not a stringified default. A `string` default
  comes back as `std::string`, an `integer` default as `uint32_t` /
  `int32_t` / `double` per range, a `boolean` default as `bool`.
- **Unknown keys** (no schema entry) pass through unchanged. The
  schema is opt-in, not a whitelist.

### Supported types

| `type=` value | Validation                                      |
|---------------|-------------------------------------------------|
| `string`      | variant alternative is `std::string`            |
| `integer`     | variant is `uint32_t` / `int32_t` / integer `double`; respects `min`/`max` |
| `float`       | variant is `double` / `uint32_t` / `int32_t`    |
| `boolean`     | variant alternative is `bool`                   |
| `opaque`      | treated as `string`                             |
| (omitted)     | always passes — same as opaque                  |

> **Schema dir default (PR #11).** ds-server now auto-loads
> `/etc/iot/ds-schemas/` when `ds-schema-dir=` isn't given. The
> canonical `iot.lua` schema is installed there by the cmake
> `install` rule.

---

## 15. Related docs

- [`protocol.md`](protocol.md) — current wire-level EMP framing
  (header, opcodes, status codes, push semantics, divergences from
  Mihini, ds-cli examples).
- [`client_api.md`](client_api.md) — application-developer guide for
  `libdatastore_client` (CMake wiring, typed values, watch styles,
  threading rules, schema deployment, error patterns, worked startup).
- [`tdd.md`](tdd.md) — phase plan + tests.
- [`../../../apps/docs/architecture.md`](../../../apps/docs/architecture.md) —
  iot-binary layout (adds a `ds-server` box once D1 lands).
- [`../../../apps/src/ds_config.cpp`](../../../apps/src/ds_config.cpp) —
  worked example of consuming the client lib from a real binary
  (now with live `watch`, PR #12).
- [`../../../apps/docs/cli.md`](../../../apps/docs/cli.md) —
  config-file Lua shape we reuse for the persistor.
- [`../../../log/L10/results.md`](../../../log/L10/results.md) — phase
  closure record, evidence files, open follow-ups.
