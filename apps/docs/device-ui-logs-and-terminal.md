# Device-ui: daemon logs & remote terminal

How two device-ui features work end-to-end: **(1)** streaming each daemon's logs
to the browser, and **(2)** the remote shell/terminal. Both are built on the same
substrate as the rest of the UI — the **data-store** as the message bus and
**iot-httpd blocking long-poll** as the transport — deliberately, because the
cloud reverse-proxy (`modules/http-server/src/handler_proxy.cpp`) is
**store-and-forward** (one request → one full response, `Connection: close`) and
**cannot carry WebSocket or SSE**. Everything here is plain request/response so it
survives the proxy hop from `console.<cloud>/dev/<endpoint>/…` to the device.

---

## 1. Daemon logs → device-ui

```
 ACE_DEBUG/ACE_ERROR (any daemon)
      │  ACE_Log_Msg_Callback sink
      ▼
 LogBuffer  ── ring buffer (200 lines) ── flush timer (own ACE_Task thread)
      │  ds.set(log.<daemon>.text)  +  ds.set(log.version, <unix ts>)
      ▼
 data-store  ── log.version bump ──▶ /api/v1/status long-poll wakes the SPA
      │
 iot-httpd  GET /api/v1/log  (merges the per-daemon text buffers, text/plain)
      ▼
 device-ui  Logs page (app-log-viewer): re-GET on each log.version bump
```

### 1a. Capture — `data_store::LogBuffer`

`modules/data-store/inc/data_store/log_buffer.hpp` + `src/log_buffer.cpp`. Each
daemon constructs one global instance:

```cpp
// e.g. iot-httpd (modules/http-server/src/main.cpp:53)
static data_store::LogBuffer g_log("httpd", "log.text", "log.level.httpd");
//                                   │        │            └ per-daemon level key (falls back to log.level)
//                                   │        └ ds key the buffer text is written to
//                                   └ short tag prepended to every line ("httpd: …")
```

- **ACE hook.** An inner `ACE_Log_Msg_Callback` overrides `log(ACE_Log_Record&)`.
  `g_log.start()` (called from `main()`, *not* the constructor — avoids static-init
  ordering) installs the sink: `ACE_Log_Msg::instance()->msg_callback(&cb)` +
  `set_flags(MSG_CALLBACK)`. Every `ACE_DEBUG`/`ACE_ERROR` line becomes
  `"<daemon>: <msg>\n"` and is pushed onto a ring buffer.
- **Ring buffer.** `std::deque<std::string>` capped at **200 lines** (mutex-guarded);
  oldest lines drop. Tracks `bytes_since_flush`.
- **Thread caveat.** `ACE_Log_Msg` is *thread-specific*. Worker/reactor threads
  (e.g. the LwM2M `svc()` thread) call `attach_current_thread()` to re-install the
  sink + pinned severity mask; `refresh_level()` (a cheap atomic-generation check)
  re-pins after a runtime level change.
- **Flush.** `open(ds, interval_sec, min_bytes)` spins a private `ACE_Reactor` on a
  dedicated `ACE_Task` "Pump" thread (same pattern as `StatsPublisher`); a timer
  fires `flush()` on the interval. `flush()` concatenates the deque and, only if
  there's new data:
  ```cpp
  ds.set(log_key,       Value{text}, 200);   // persistent per-daemon buffer (~16 KB)
  ds.set("log.version", Value{unix_ts}, 100);// bump → wakes the UI long-poll
  ```
  It uses **`set`** (persistent), not `set_volatile`. Cadence: iot-httpd
  `open(ds, 10, 200)` (10 s / 200-byte threshold); containerd `open(ds, 5, 1)`.
- **Level.** `apply_level(ds)` reads `{level_key, "log.level"}` (first non-empty
  wins) and sets the ACE priority mask **cumulatively** (each level enables itself
  + everything more severe): `ERROR` ⊂ `WARNING` ⊂ `INFO` (default) ⊂ `DEBUG`.
  Applied at startup and re-applied on a `log.level*` watch event (hot-reload).

### 1b. ds keys (`modules/data-store/schemas/iot.lua`)

| Key | Access | Role |
|---|---|---|
| `log.version` | Viewer (int) | **bump key** — flush writes a unix-ts here; the UI long-poll watches it |
| `log.level` | Admin (str) | global level fallback (default `INFO`) |
| `log.level.{httpd,lwm2m,lwm2m.bs,lwm2m.dm,cloudd,vpn,dtls,vehicle,mqtt}` | Admin | per-daemon level (`""` = inherit global) |
| `log.text` | Viewer (str) | iot-httpd buffer (note: **`log.text`**, not `log.httpd.text`) |
| `log.{cloudd,lwm2m,lwm2m.bs,lwm2m.dm,vehicled,mqttd}.text` | Viewer | per-daemon buffers (~200 lines each) |

Daemons that register a `LogBuffer` (tag → log_key → level_key):

| Daemon | tag | log_key | level_key |
|---|---|---|---|
| iot-httpd | `httpd` | `log.text` | `log.level.httpd` |
| iot-cloudd | `cloudd` | `log.cloudd.text` | `log.level.cloudd` |
| lwm2m client | `lwm2m` | `log.lwm2m.text` | `log.level.lwm2m` |
| iot-containerd | `containerd` | `log.containerd.text` | `log.level.container` |
| iot-vehicled | `vehicled` | `log.vehicled.text` | `log.level.vehicle` |
| iot-mqttd | `mqttd` | `log.mqttd.text` | `log.level.mqtt` |

### 1c. Serving (iot-httpd)

- **`GET /api/v1/log`** (`handler.cpp`) — merges `{log.text, log.cloudd.text,
  log.lwm2m.text, log.lwm2m.bs.text, log.lwm2m.dm.text}` into one `text/plain`
  snapshot. No long-poll on this route itself.
- **Long-poll wake** rides the shared **`GET /api/v1/status`** handler, which
  registers `ds->watch("log.version", …)` among its watches. A `log.version` bump
  (or the poll timeout) wakes the status stream and echoes `log.version` back, so
  the SPA knows to re-GET `/api/v1/log`. No dedicated logs long-poll needed.

### 1d. device-ui Logs page

`iot-ui/src/app/log-level/log-viewer.component.ts` (`app-log-viewer`) — reached via
the **Logs** menu.

- **Text:** on init GETs `/api/v1/log` (`responseType:'text'`); subscribes to
  `ds.observe('log.version')` and re-fetches on each bump when auto-refresh is on.
  Toolbar: Refresh / Export (client-side `.txt`) / Clear / auto. The server
  pre-merges, so there is **no per-daemon text selector** — the view is the merged
  stream.
- **Levels:** dropdowns for `log.level` (All) + `log.level.{httpd,lwm2m,vpn,dtls}`,
  written via `POST /api/v1/db/set`; Admin-only.

### 1e. Known gaps (as of this writing)

- `log.containerd.text` / `log.level.container` are **used by iot-containerd but not
  declared** in `iot.lua`'s log section (they work as ad-hoc keys, but aren't in
  the schema).
- The `/api/v1/log` merge **omits** `log.vehicled.text`, `log.mqttd.text`, and
  `log.containerd.text` — those buffers are populated but not shown in the merged
  view. (Read them directly via `/api/v1/db/get` if needed.)

---

## 2. Remote terminal → device-ui

```
 browser (xterm.js)
   POST /api/v1/shell/open {cols,rows}         → {sid}
   GET  /api/v1/shell/output?sid=&timeout=25   ← {data(b64), closed}   (long-poll loop)
   POST /api/v1/shell/input  {sid, data(b64)}  → keystrokes
   POST /api/v1/shell/resize {sid, cols, rows}
   POST /api/v1/shell/close  {sid}
        │
 iot-httpd  handler_shell.cpp ── ShellManager / ShellSession
        │  forkpty() → PTY master (O_NONBLOCK) ↔ `/bin/sh -i` child
        ▼
   pty master: write_input() ← input;  read_output(poll) → output (base64)
```

### 2a. Server side

`modules/http-server/inc/shell.hpp`, `src/shell.cpp` (`ShellManager` / `ShellSession`
+ base64), `src/handler_shell.cpp` (routes). Wired in `main.cpp`:
`ShellManager shell_mgr(maxSessions, idleSec)` + `install_shell_handlers(...)`.

Routes (all under `/api/v1/shell/`, all gated — see 2b):

| Route | Body → Response | Notes |
|---|---|---|
| `POST /open` | `{cols,rows}` → `{ok,sid}` | `forkpty`s a new PTY; 503 if at the session cap |
| `GET /output` | `?sid=&timeout=N` → `{ok,sid,data(b64),closed}` | **long-poll**; `timeout` clamped 0..30 (default 25) |
| `POST /input` | `{sid,data(b64)}` → `{ok}` | base64-decode → write to PTY master; 404 if no session |
| `POST /resize` | `{sid,cols,rows}` → `{ok}` | `TIOCSWINSZ` |
| `POST /close` | `{sid}` → `{ok}` | SIGHUP + reap |

- **PTY.** `ShellManager::open` → `forkpty(&master,…,&winsize)`. Child:
  `TERM=xterm-256color`, `execl($SHELL or /bin/sh, "-i")`. Parent: master set
  `O_NONBLOCK|FD_CLOEXEC`, wrapped in `shared_ptr<ShellSession>`, keyed by a
  128-bit hex **sid** from `/dev/urandom`.
- **Output** (`read_output`): `poll(POLLIN, timeout)`, drains up to 256 KiB/ poll in
  16 KiB reads; `read()==0`/EIO → `closed`. No per-session reader thread — output is
  drained lazily inside the poll, slotting into the existing blocking-long-poll
  worker model exactly like `/api/v1/db/get`.
- **Input** (`write_input`): loop `write()` with EINTR/EAGAIN handling.
- **Lifecycle.** `~ShellSession` closes the master (SIGHUP to the child group) +
  SIGKILL + `waitpid`. `close(sid)` erases + `signal_hangup()` so an in-flight
  `/output` poll wakes `closed`. A background **idle reaper** (30 s tick) closes any
  session idle beyond `http.shell.idle.sec`; every read/write/resize `touch()`es
  `last_active`. `find()` returns a `shared_ptr` so a concurrent close can't free a
  session mid-poll.

### 2b. Gating — off by default, Admin-only

`gate()` runs on **every** `/shell/*` route:
1. **`http.shell.enabled`** (read per request; default **false**) → 403
   `"shell disabled"` when off.
2. Session access must be **Admin** (auth disabled ⇒ treated as Admin) → else 403.

ds keys (`modules/http-server/schemas/http.lua`, all Admin):

| Key | Default | Role |
|---|---|---|
| `http.shell.enabled` | **false** | master switch (per-request) |
| `http.shell.idle.sec` | 300 (30..3600) | idle-reap timeout |
| `http.shell.max.sessions` | 4 (1..32) | concurrent session cap |

> The shell runs as the **iot-httpd service user** (DynamicUser, group `iot`) — *not*
> root. (`shell.hpp`'s "root shell" header comment is stale; the deployed reality is
> the service user.) Each open terminal holds **one blocking long-poll worker**, so
> the schema advises `http.workers >= 2` before enabling it.

### 2c. device-ui Terminal component

`iot-ui/src/app/shell/shell.component.ts` (`app-shell`) — **Terminal** menu, visible
only when `http.shell.enabled` (via `ds.observe`) **and** the session is Admin.

- Renders with **xterm.js** (`Terminal` + `FitAddon`). `term.onData` →
  `POST /shell/input` (base64; local echo comes back via the poll).
- `open()` → `POST /shell/open` → stores `sid` → starts `poll(sid)`.
- **Long-poll loop:** a single outstanding `GET /shell/output?sid=&timeout=25`; on
  response, write `b64→bytes` to xterm and, if `!closed`, re-issue (recursive).
  `closed` ends it; errors back off ~1.5 s and retry. A `this.sid !== sid` guard
  drops stale responses after a "New session".
- **Resize:** `window:resize` → `FitAddon.fit()` → `POST /shell/resize`.
- Cleanup: `ngOnDestroy` → `POST /shell/close` + `term.dispose()`.
- base64 both ways because PTY bytes may not be valid UTF-8.

### 2d. Why long-poll, not WebSocket

`handler_proxy.cpp` is a **store-and-forward** reverse proxy: each `/dev/<endpoint>/…`
request opens a fresh upstream TCP socket to the device's tunnel IP, forwards with
`Connection: close`, and reads the full response to EOF — **no streaming/upgrade
path**, so WebSocket/SSE can't traverse it. Its recv timeout is **75 s** ("> the
longest device long-poll"). The shell therefore caps `/output` at **30 s** (well
under 75 s) so every long-poll returns as a complete HTTP response the proxy can
relay. This is the same reason the logs, `/status`, and `/api/v1/db/get` all use
bounded long-poll rather than push.
