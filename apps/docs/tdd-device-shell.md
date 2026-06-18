# TDD — Device-UI Terminal (remote shell)

A browser terminal in the device-ui that drives a real shell on the device
(forkpty + interactive `/bin/sh`), reachable both on the LAN and through the
cloud per-device reverse proxy.

## Why long-poll, not WebSocket

The device-ui is served two ways:

1. **Directly** by the device `iot-httpd` at `device-ip:8080` (same origin).
2. **Through the cloud** at `/dev/<ep>/…` via `modules/http-server/src/handler_proxy.cpp`,
   which proxies over the VPN tun.

That cloud proxy is **store-and-forward**: it `recv`s the *entire* device
response into a string, rewrites `<base href>` / `Set-Cookie` / `Location`,
then returns it. It has **no HTTP `Upgrade` handling and no streaming flush**.
A WebSocket (`101 Switching Protocols`) or SSE stream would never flush
through it — it would hang until the proxy's 75 s recv timeout. So WebSocket
would only work on the LAN, defeating remote management of a field device.

Long-poll already survives the proxy (it backs `/api/v1/status` and
`/api/v1/db/get?timeout=`; the proxy recv timeout is deliberately `> 75 s`).
So the Terminal is split-duplex over plain request/response:

| Route | Method | Purpose |
|-------|--------|---------|
| `/api/v1/shell/open`   | POST | forkpty a shell `{cols,rows}` → `{sid}` |
| `/api/v1/shell/output` | GET  | long-poll PTY output → `{data(b64), closed}` |
| `/api/v1/shell/input`  | POST | write keystrokes `{sid, data(b64)}` |
| `/api/v1/shell/resize` | POST | `TIOCSWINSZ` `{sid, cols, rows}` |
| `/api/v1/shell/close`  | POST | SIGHUP + reap `{sid}` |

PTY traffic is raw bytes (control codes, possibly multibyte UTF-8) and can't
ride a JSON string verbatim, so it is **base64** in both directions; the
browser deals in `Uint8Array` and lets xterm do its own UTF-8 decode.

## Backend (`modules/http-server`)

- `inc/shell.hpp` / `src/shell.cpp` — `ShellSession` (one forkpty'd shell) +
  `ShellManager` (session map keyed by a 128-bit `/dev/urandom` id + idle
  reaper thread).
  - Output is drained **lazily inside the `/output` long-poll** — the kernel
    PTY buffer (~64 KiB) holds bytes between polls. No per-session reader
    thread and nothing registered with the ACE reactor; it slots into the
    existing blocking-long-poll worker model exactly like `/api/v1/db/get`.
  - Master fd is `O_NONBLOCK`; `/output` `poll()`s for readiness then reads
    up to 256 KiB per response (a flood can't grow the body unbounded — the
    rest comes next poll). EOF / `EIO` ⇒ `closed=true`.
  - `~ShellSession` closes the master (kernel SIGHUPs the child), `SIGKILL`
    backstops, `waitpid` reaps — no zombies.
  - Reaper SIGHUPs + drops sessions idle longer than `http.shell.idle.sec`.
- `src/handler_shell.cpp` — `install_shell_handlers()`. Every route is gated:
  **`http.shell.enabled` (read from ds per request) AND Admin** (session
  cookie, same check as `/api/v1/update/upload`). Flipping the flag off 403s
  new requests immediately and kills existing sessions on their next poll.
- `src/main.cpp` — constructs one `ShellManager` (tunables from ds), installs
  the handlers after the proxy handler.
- `CMakeLists.txt` — links `util` (forkpty) on Linux.

## Config (`schemas/http.lua`, Project Rule 2)

| Key | Type | Default | Notes |
|-----|------|---------|-------|
| `http.shell.enabled`      | boolean | **false** | master switch; operator opt-in |
| `http.shell.idle.sec`     | integer | 300 | reap idle sessions |
| `http.shell.max.sessions` | integer | 4   | cap (each ties up one long-poll worker) |

`http.shell.enabled` is a normal Admin-writable ds key (schema default
false), so it is toggleable two ways:

- **From the UI** — HTTP Config page → *Remote Shell* checkbox
  (`toggleShell()` → `ds.write('http.shell.enabled')`). The backend re-reads
  it per request, so it takes effect at once; the **Terminal** sidebar item
  appears/disappears live because `main.component` observes the key off the
  shared store (it is prefetched in `DataStoreService.ALL_KEYS`).
- **From the shell** — `ds-cli set http.shell.enabled true`.

Either way, give the server enough workers first (each open terminal holds
one blocking long-poll worker), then restart — `http.workers` is not
hot-reloaded:

```
ds-cli set http.workers 4        # restart iot-httpd
```

## Frontend (`iot-ui`)

- `package.json` — `xterm` + `xterm-addon-fit`; `angular.json` adds
  `xterm/css/xterm.css` globally (xterm builds DOM dynamically, so scoped
  component styles wouldn't reach it) and allows the CommonJS deps.
- `src/common/httpsvc.service.ts` — `shellOpen/Output/Input/Resize/Close`
  off the same `api` base, so it works same-origin and through the cloud
  proxy unchanged.
- `src/app/shell/shell.component.ts` — mounts xterm, fits on resize (→ resize
  POST), `onData` → input POST, one outstanding `/output` long-poll that
  re-subscribes per response (guarded by `sid` so a late response from a
  prior session can't clobber the current one). Closes the session on
  `ngOnDestroy`.
- `main.component` — the **Terminal** sidebar item shows only when
  `http.shell.enabled` **and** the user is Admin (`navMenus` getter).

## Security

This is a remote shell that runs as the `iot-httpd` service user
(`DynamicUser=yes` — **not root**, so privileged ops like `ping`/raw sockets,
`opkg`, and writes under `/etc` are unavailable). It is still the largest attack
surface in the device-ui.
Mitigations: off by default; Admin-only; gated per request (instant kill
switch); idle-reaped; session-capped; audit-logged via `ACE_DEBUG` on
open/close/reap. It rides the existing session-cookie auth and (where
configured) the same TLS the rest of the API uses.

## Status

Backend compiles + links and the SPA builds (production) in the standard
podman/node images. Live device validation (forkpty under busybox `ash`,
xterm round-trip on a real RPi) pending a reflash.
