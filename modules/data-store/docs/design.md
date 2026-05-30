# Local Data Store — Design

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

---

## 5. Threading

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

Settable on the iot CLI:

| Flag           | Default                            | Purpose                                      |
|----------------|------------------------------------|----------------------------------------------|
| `ds-enable=`   | `false`                            | When `true`, start the data store at boot.   |
| `ds-socket=`   | `/var/run/iot/data_store.sock`     | Listen path; created with 0660.              |
| `ds-store=`    | `/var/lib/iot/data_store.lua`      | Persistent backing file.                     |
| `ds-mode=`     | `write-through`                    | Reserved; future `batch=100ms` value.        |

---

## 10. CLI test client

A thin CLI client lives at `apps/src/data_store_client.cpp`. Same
heredoc-friendly stdin shape as the existing readline CLI. Compiled
as a separate target (`ds-cli`) so a deployment can ship it without
the full iot binary for debugging.

```
$ ds-cli /var/run/iot/data_store.sock
ds> set foo bar
ok
ds> get foo
foo=bar
ds> watch foo
watching foo
ds> ...
[event] foo changed bar → quux
```

The CLI is a smoke-harness convenience; integrations talk JSON
directly.

---

## 11. Open questions (track to closure)

| Q   | Question                                          | Resolution path                           |
|-----|---------------------------------------------------|-------------------------------------------|
| Q1  | Should `remove` (key un-register) imply a final notify-of-removal? | Default no per §6. Re-open if a client team asks. |
| Q2  | Per-key max value size?                            | Pick after the first real consumer lands; default cap 64 KiB until then. |
| Q3  | ~~In-process accessor for bootstrap / DM?~~        | **Closed: no.** Architecture pivoted to a separate-binary model. The iot binary talks to `ds-server` via the same unix-socket path as any other client; no in-process shortcut. |

---

## 12. Related docs

- [`tdd.md`](tdd.md) — phase plan + tests.
- [`../../../apps/docs/architecture.md`](../../../apps/docs/architecture.md) —
  iot-binary layout (adds a `ds-server` box once D1 lands).
- [`../../../apps/docs/cli.md`](../../../apps/docs/cli.md) —
  config-file Lua shape we reuse for the persistor.
