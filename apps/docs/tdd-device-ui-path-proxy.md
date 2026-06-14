# TDD — Per-device path-scoped reverse proxy for the device UI

Status: **DESIGN (for review)** · Target: cloud (`iot-httpd`) + device UI (`iot-ui`)
· Author: 2026-06-14

## 1. Goal

Serve every device's web UI through the **cloud's single HTTPS origin** under a
per-device path:

```
  https://<cloud>/dev/<endpoint>/...   ──►  (over the VPN tun)  ──►  dev_tun_ip:8080
```

…instead of today's per-device **published port + nftables DNAT**
(`http://<cloud>:<proxy_port>/`). This is the long-term replacement flagged in
PR #183.

### Why

| Today (port + DNAT) | Problem |
|---------------------|---------|
| Each device UI on a distinct **port** of the cloud host | Cookies are **not** port-scoped → device↔device session clobber (the residual bug after #183). |
| One published port + one `docker-proxy` **per device** | Hard ceiling (~50, `cloud.vpn.proxy.port.start/end`); wide ranges exhaust the host. |
| Proxy ports are **plain HTTP** | Device UI traffic to the operator is unencrypted on the wire to the cloud. |
| `iot-cloudd` rebuilds nftables DNAT per device | Extra moving part (the `flush table` / `dev_tun_ip` churn we've already had bugs in). |

Path-scoping fixes all four: **one origin, one TLS cert, one published port
(443), unlimited devices, per-device cookie isolation, no DNAT.**

## 2. URL scheme

```
/dev/<ep>/                     → device index.html (SPA)
/dev/<ep>/<asset>              → device static asset (main.js, styles.css, …)
/dev/<ep>/api/v1/...           → device REST API
/webui/...                     → cloud UI (unchanged)
/api/v1/...                    → cloud API (unchanged)
```

`<ep>` is the **URL-encoded endpoint name** from `cloud.endpoints` (e.g.
`urn:dev:gateway-42` → `urn%3Adev%3Agateway-42`). The proxy resolves it to
`dev_tun_ip` (the live openvpn-assigned address). Unknown/`dev_tun_ip`-less
endpoint → 502 "VPN down" (mirrors the Launch-UI gating from #178).

## 3. The three rewrites (the whole trick)

The device SPA is built once with `<base href="/">` and an **empty `apiUrl`**
(verified: `iot-ui/.../httpsvc.service.ts` builds `${apiUrl}/api/v1/...`, and
modern Angular emits **relative** `<script src="main.js">` tags). That makes it
servable under any prefix with exactly three response/request rewrites in the
proxy — **no per-device rebuild**:

1. **Request path strip.** `/dev/<ep>/foo` → forward `GET /foo` to
   `dev_tun_ip:8080`. (`/dev/<ep>/` → `/`.)
2. **`<base href>` injection.** In the `index.html` body, rewrite
   `<base href="/">` → `<base href="/dev/<ep>/">`. The browser then resolves the
   relative `<script>`/`<link>` tags and the router to `/dev/<ep>/…`, which come
   back to the proxy and strip cleanly.
3. **`Set-Cookie` Path rewrite.** Rewrite the device's
   `Set-Cookie: iot-session=…; Path=/` → `Path=/dev/<ep>/`. **This is the cookie
   fix:** device A's cookie is scoped to `/dev/A/`, device B's to `/dev/B/`, the
   cloud's to `/` (or `/webui/`) — the browser never sends one to another.
   Device↔device and cloud↔device collisions both vanish, regardless of cookie
   *name* (so this also supersedes #183's name hack for the device side).

Plus one **device-SPA one-liner** (the only client change): make API calls
base-relative so `/dev/<ep>/` pages call the *device* API, not the cloud's.
`apiUrl` becomes computed from the document base instead of `''`:

```ts
// httpsvc.service.ts — was: private api = environment.apiUrl  (== '')
private api = environment.apiUrl ||
              new URL(document.baseURI).pathname.replace(/\/$/, '');
// base '/'         → ''            → `${api}/api/v1` = /api/v1            (direct)
// base '/dev/ep/'  → '/dev/ep'     → `${api}/api/v1` = /dev/ep/api/v1     (proxied)
```

Zero call-site changes (every call already does `${this.api}/api/v1/...`), and
it's a no-op for the direct (un-proxied) device deployment. The same one-liner
goes in the cloud UI's service for symmetry (harmless; base is `/webui/`).

> Note `Location:` headers (3xx redirects) would also need prefixing — but the
> device UI is a client-routed SPA and its httpd doesn't emit server-side
> redirects to absolute paths today; the proxy will still defensively prefix any
> `Location: /…` to be safe.

## 4. Components

### 4.1 Proxy route in `iot-httpd` (cloud)

Home = **`iot-httpd`**, not a new daemon: it already owns the HTTPS origin, TLS,
the router, auth, and the worker pool. Extend the existing (currently
resolve-only) `modules/server/web` `DeviceProxy`:

- **Resolve `<ep>` → `dev_tun_ip`** from the `cloud.endpoints` ds JSON (httpd
  already reads ds), *not* the in-memory `EndpointRegistry` (which lives in
  `iot-cloudd`, a different process). Use `dev_tun_ip` only (tunnel-up signal,
  per #178); empty → 502.
- **Outbound HTTP client** (the missing half of `DeviceProxy`): `ACE_SOCK_Connector`
  → `dev_tun_ip:<ui_port>` (`cloud.proxy.device.ui.port`, default 8080), send the
  rewritten request line + headers + body, read the response, apply the Set-Cookie
  / base-href rewrites, return it. The pattern already exists in-tree
  (`apps/cloud/server/src/main.cpp::poll_vpn_client_ips` uses
  `ACE_SOCK_Connector`/`ACE_SOCK_Stream`). All socket I/O stays in the ACE layer.
- **Wire-up:** register a prefix route for `/dev/` in the router, dispatched
  *before* static serving (`router.cpp`), wrapped in the cloud auth guard (§4.2).

### 4.2 Auth (two layers, v1)

- **Cloud gate:** `/dev/<ep>/*` requires a valid **cloud** operator session (the
  proxy route is behind `with_auth`). Random internet clients can't reach device
  UIs even though they're on the public 443.
- **Device session:** the device httpd still runs its own auth; the operator logs
  into the device once, and that cookie is **path-scoped to `/dev/<ep>/`** by
  rewrite (§3.3). So two logins (cloud, then device) in v1.
- **Future (noted, not v1):** SSO — the proxy injects a device session minted
  from the cloud identity so the second login disappears.

### 4.3 SSRF / safety

The proxy connects **only** to a `dev_tun_ip` that appears in `cloud.endpoints`
and lies inside the VPN subnet (`cloud.vpn.subnet`). `<ep>` that doesn't resolve
→ 404; resolves but no `dev_tun_ip` → 502. No user-controlled host/port ever
reaches `ACE_SOCK_Connector`. The upstream port is the fixed
`cloud.proxy.device.ui.port`, not from the URL.

## 5. Concurrency & long-poll

The device UI uses **long-poll** (`/api/v1/status?timeout=…`,
`/db/get?key=…&timeout=…`) up to ~60 s. The proxy call is **synchronous inside a
worker-pool thread** (httpd already has `http.workers`): a worker is occupied for
the duration of the upstream request.

- Set the upstream socket recv timeout > the longest long-poll (e.g. 75 s).
- Size `http.workers` for `(operators × concurrent device tabs)` long-poll slots,
  plus cloud traffic. Document the rule of thumb.
- **Scale path (noted):** if worker-occupancy becomes a ceiling, move the proxy
  to an async reactor-driven pump (no thread parked per in-flight request). v1 is
  synchronous for simplicity; the worker pool already exists.

Body handling: buffer the full upstream response before returning (device assets
are ~1–2 MB, long-poll bodies are small JSON). Streaming/chunked is a later
optimization. Send `Connection: close` upstream (connection-per-request) for v1.

## 6. Migration from port + DNAT

Phased, non-breaking:

1. **Ship the path proxy** in `iot-httpd` (behind cloud auth) + the device-SPA
   base-relative one-liner. Both old (`:proxy_port`) and new (`/dev/<ep>/`) paths
   work simultaneously.
2. **Flip "Launch UI"** in the cloud UI from
   `http://<host>:<proxy_port>/` to `/<base>/dev/<encodeURIComponent(ep)>/`
   (same-origin, HTTPS). Gating stays `registered && dev_tun_ip`.
3. **Deprecate the port plane** in a follow-up once verified: stop allocating
   `proxy_port`, stop publishing `10000-10050`, and remove the `rebuild_device_dnat`
   path + the `modules/server/dnat` usage from `iot-cloudd`. `proxy_port` can stay
   in `cloud.endpoints` (ignored) for one release to avoid a schema break.

This means the deep-dependency change (dropping DNAT/published ports) is a
separate, reversible PR after the proxy is proven.

## 7. What changes (files)

**Cloud (`iot-httpd` + `modules/server/web`)**
- `modules/server/web/proxy.{hpp,cpp}` — add the outbound HTTP client + the three
  rewrites; resolve from `cloud.endpoints` (ds) instead of `EndpointRegistry`.
- `modules/http-server/src/router.cpp` (+ `handler.cpp`) — register the `/dev/`
  prefix route before static serving, behind `with_auth`.
- `modules/http-server/src/main.cpp` — construct the proxy with the ds handle +
  `cloud.proxy.device.ui.port`; link `libserver_web` into `iot-httpd` (build).
- `apps/cloud/Dockerfile` — link the web module into `iot-httpd`.

**Cloud UI**
- `endpoint-list.component.ts` — "Launch UI" → `'/dev/' + encodeURIComponent(ep) + '/'`
  (same-origin), keep the `registered && dev_tun_ip` gate; drop `windowHost:proxy_port`.

**Device UI (and cloud UI for symmetry)**
- `iot-ui/src/common/httpsvc.service.ts` (+ the cloud copy) — base-relative
  `apiUrl` one-liner (§3).

**Follow-up PR (after verification)**
- `apps/cloud/server/src/main.cpp` — stop allocating `proxy_port`, drop
  `rebuild_device_dnat`; `apps/cloud/docker-compose.yml` / `run.sh` — stop
  publishing the proxy range. Keep 443 (and 1194) published.

## 8. Failure modes

| Case | Behaviour |
|------|-----------|
| `<ep>` unknown in `cloud.endpoints` | 404 |
| Known but no `dev_tun_ip` (tunnel down) | 502 + "VPN down" body (UI already gates the link) |
| Upstream connect refused / device httpd down | 502 |
| Upstream timeout (> long-poll window) | 504; client long-poll just retries |
| Not cloud-authenticated | 401 (cloud auth guard) — never reaches the device |
| Oversized body | cap + 413 (configurable) |

## 9. Test plan

- **Unit:** `DeviceProxy` path-strip + `<ep>`→`dev_tun_ip` resolve from a fixed
  `cloud.endpoints` JSON; `Set-Cookie` Path rewrite; `<base href>` rewrite.
- **httpd integration:** a fake upstream (local listener) returning index.html +
  a Set-Cookie; assert the proxied response has `<base href="/dev/<ep>/">`,
  `Path=/dev/<ep>/`, and that `/dev/<ep>/api/v1/...` reaches the upstream
  `/api/v1/...`.
- **e2e (podman cloud+device):** Launch UI from cloud Endpoints → device SPA
  loads under `/dev/<ep>/`, device login works, and **logging into a second
  device does not log you out of the first** (the cookie-isolation acceptance).
- **Long-poll:** confirm `/status` long-poll holds open through the proxy and
  updates live; confirm a worker isn't leaked on client disconnect.

## 10. Decisions

1. **Home = `iot-httpd` proxy route** ✅ (recommended/accepted) — self-contained,
   one TLS origin, reuses auth + workers; no new dependency or daemon.
2. **Sync (worker-pool) v1** ✅ (recommended/accepted) — + a documented
   `http.workers` sizing rule; async reactor pump is the scale follow-up.
3. **Auth = cloud-gate + device login (two logins)** ✅ (confirmed 2026-06-14) —
   `/dev/<ep>/*` behind the cloud session; device keeps its own (path-scoped)
   login. SSO injection is a later enhancement.
4. **Migration = phased** ✅ (recommended/accepted) — ship the proxy alongside
   the port plane, flip Launch UI, then remove DNAT/published ports in a separate
   follow-up PR once verified.
5. **URL form = `/dev/<urlencoded-endpoint>/`** ✅ (recommended/accepted) —
   human-readable, no new id to allocate; resolved against `cloud.endpoints`.
