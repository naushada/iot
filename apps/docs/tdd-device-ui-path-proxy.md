# TDD — Per-device path-scoped reverse proxy for the device UI

Status: **ENABLED via shared netns (see §netns)** · Target: cloud (`iot-httpd`)
+ device UI (`iot-ui`) · Author: 2026-06-14

## §netns — the proxy must run where the tun is (RESOLVED)

The proxy runs inside `iot-httpd`, but the OpenVPN `tun0` (10.9.0.1) lives in the
`iot-cloudd` container. Initially `iot-httpd` was a separate bridge container
with **no route to 10.9.0.0/24**, so the proxy's `ACE_SOCK_Connector` to
`dev_tun_ip:8080` failed → **504** (and unauth → 302 to `/webui/`, which looked
like "Launch UI opens the Cloud UI"). Verified live; also verified that a process
*inside* `iot-cloudd`'s netns reaches `10.9.0.2:8080` fine.

**Resolution (shipped):** `iot-httpd` now joins `iot-cloudd`'s network namespace
— `network_mode: "service:iot-cloudd"` in `apps/cloud/docker-compose.yml`, with
the published HTTP port moved onto `iot-cloudd` (the netns owner). Mirrors the
device stack's httpd↔openvpn-client sharing (#175). httpd now sees `tun0` and
reaches `10.9.0.x`, so `/dev/<ep>/` works; Launch UI points back at it.

> **Deploy requirement:** this needs the **updated compose** (shared netns) AND
> the image with the proxy. Pulling a new image without the new compose leaves
> httpd in its own netns → 504 again. Redeploy both.

The port-based DNAT (`proxy_port` range) stays published for one transition
release as a fallback; removing it is the phase-2 follow-up.

---

(original design follows)

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

---

## Appendix A — Primer: how the cloud reaches the device's `:8080` over the VPN tunnel

*This appendix is a from-scratch explanation for someone new to the codebase.
It re-tells §1–§4 at a slower pace, one layer at a time. Nothing here is new
behaviour — it's the same flow the code above implements, spelled out.*

### A.1 The problem in one sentence

The device's web server runs on **port 8080**, but the device sits **behind a
home router (NAT)** — it has a private address like `192.168.1.7` and **no
public IP**, so the cloud **cannot dial it directly** from the internet. The VPN
tunnel exists to solve exactly this: it gives the cloud a private, always-open
"back door" to each device.

### A.2 First, clear up "localhost:8080"

It's natural to say *"the device UI runs on localhost:8080."* That's true from
the **device's own point of view**, but it's not the whole story, and the
difference is the crux of this whole design.

The device httpd binds to **`0.0.0.0:8080`** (config key `http.listen.ip`,
default `0.0.0.0`), which means *"accept connections arriving on **any** network
interface,"* not just the loopback one. So the same server is reachable at:

| Address | Who can use it |
|---------|----------------|
| `127.0.0.1:8080` | only a process **on the device itself** (true "localhost") |
| `192.168.1.7:8080` | anyone on the **home LAN** |
| `10.9.0.7:8080` | anyone on the **VPN** — *this is the cloud's door in* |

That last address — `10.9.0.7` — only exists **because of the tunnel**. It's a
*virtual* address the VPN hands the device. The cloud connects to **that**, and
because the httpd listens on `0.0.0.0`, the connection is accepted just like a
LAN one. If the httpd bound to `127.0.0.1` only, this would all be impossible —
the tunnel would deliver the packet and the kernel would refuse it.

### A.3 The tunnel is a pipe between two virtual addresses

Think of the VPN (OpenVPN) as a **physical pipe** welded between the cloud and
the device. Each end of the pipe is a **virtual network card** called `tun0`:

- the **cloud** end of the pipe is `tun0 = 10.9.0.1` (fixed — the concentrator),
- **each device** gets its own end, e.g. `tun0 = 10.9.0.7` (the `dev_tun_ip`).

```
        CLOUD  (public IP, e.g. 65.20.74.23)                  DEVICE (behind home NAT — no public IP)
  ┌──────────────────────────────────────────┐          ┌──────────────────────────────────────────┐
  │  iot-httpd  (the reverse proxy)           │          │  device httpd  →  listening 0.0.0.0:8080  │
  │      │ connect() to 10.9.0.7:8080         │          │        ▲  kernel delivers to :8080         │
  │      ▼                                    │          │        │                                   │
  │   tun0 = 10.9.0.1  ╞══════ THE VPN TUNNEL (one welded pipe) ══════╡  tun0 = 10.9.0.7              │
  │      │  OpenVPN encrypts everything        │  the encrypted pipe   │        ▲  OpenVPN decrypts     │
  │      ▼                                    │  rides the real        │        │                       │
  │   eth0 (public)  ─────────────────────────┼── internet, port 1194 ─┼──► router/NAT ──► eth0/wlan0 ─┘
  └──────────────────────────────────────────┘                        └──────────────────────────────┘
```

Two important consequences of the "pipe" picture:

1. **To the cloud's software, `10.9.0.7` looks like a normal, routable host.**
   The proxy just opens an ordinary TCP socket to `10.9.0.7:8080`. It does **not**
   know or care that a VPN is involved — the operating system's routing table
   sends anything for `10.9.0.0/16` into `tun0`, and OpenVPN takes it from there.
2. **The device made the pipe, not the cloud.** Because the device is behind
   NAT, *it* dials **out** to the cloud's OpenVPN server (`<cloud-public>:1194`)
   and holds that connection open. Once open, the pipe carries traffic **both
   ways** — so the cloud can now originate connections *toward* the device even
   though it could never have dialed in cold.

### A.4 What actually travels inside the pipe (the minute detail)

When the proxy calls `connect()` to `10.9.0.7:8080`, the kernel produces an
ordinary TCP/IP packet. OpenVPN, sitting on `tun0`, grabs that packet,
**encrypts it, and wraps it inside a second packet** addressed to the device's
*real* public IP on port 1194. That outer packet is what crosses the internet.
This wrap-a-packet-inside-another-packet is called **encapsulation**:

```
  (1) The proxy's packet, as the cloud kernel builds it:
          inner IP/TCP   src 10.9.0.1   →   dst 10.9.0.7 : 8080     ("please talk to the device UI")

  (2) OpenVPN grabs it at tun0 and encapsulates + encrypts:
      ┌───────────────────────────────────────────────────────────────────────┐
      │ OUTER  IP/UDP   src <cloud-public>   →   dst <device-public> : 1194     │  ← this is what the
      │   ┌───────────────────────────────────────────────────────────────┐   │    internet actually
      │   │  🔒 encrypted blob  =                                          │   │    carries; routers
      │   │       the entire inner packet:  TCP 10.9.0.1 → 10.9.0.7:8080   │   │    only see the outer
      │   └───────────────────────────────────────────────────────────────┘   │    envelope
      └───────────────────────────────────────────────────────────────────────┘

  (3) It reaches the device's router, which NATs it in to the device; the device's
      OpenVPN decrypts the blob, recovers the inner packet, and pushes it out its
      own tun0. The device kernel sees "a packet for 10.9.0.7:8080" and delivers
      it to the httpd. The reply retraces the pipe in reverse.
```

Nobody in the middle (the ISP, the coffee-shop Wi-Fi, the backbone routers) can
read the inner packet — they only see an encrypted UDP/TCP stream between two
public IPs on port 1194. That's the "private" in VPN.

> **Outer transport = OpenVPN on port 1194.** Today the outer envelope is TCP;
> switching it to UDP is a parked improvement (see the VPN-UDP-transport note).
> It doesn't change anything in this document — the *inner* connection to
> `:8080` is always TCP either way.

### A.5 The TCP connection, step by step

"The cloud initiates a TCP connection through the tunnel" means a normal TCP
**3-way handshake** happens between the cloud proxy and the device httpd — every
packet of it just rides the encrypted pipe from §A.4. Reading left-to-right is
the cloud→device direction:

```
  cloud proxy            cloud tun0 (OpenVPN)          device OpenVPN (tun0)        device httpd :8080
      │  connect()             │                             │                            │
      │ ─ SYN →10.9.0.7:8080 ─►│ encrypt, send over 1194 ═══════════════════►  decrypt ─► │ SYN  (in listen backlog)
      │                        │                             │                            │
      │ ◄──────── SYN-ACK ─────┤◄══════════ encrypt back ════════════════════════ emit ◄─ │ SYN-ACK
      │ ─ ACK ────────────────►│ ═══════════════════════════════════════════════════════►│ ESTABLISHED ✓
      │                        │                             │                            │
      │ ─ "GET /api/v1/... " ─►│ (HTTP request bytes, same encrypted pipe) ═════════════► │ handler runs
      │ ◄─────── "200 OK\r\n\r\n{...}" ◄══════════════════════════════════════════════════│ response
      │  close()               │                             │                            │
```

In code this is the whole of `upstream_exchange()` in
`modules/http-server/src/handler_proxy.cpp`:

```cpp
ACE_INET_Addr addr(port, ip.c_str());   // ip = "10.9.0.7", port = 8080
ACE_SOCK_Connector conn;
ACE_SOCK_Stream    stream;
ACE_Time_Value ct(5, 0);                 // give up if the SYN-ACK doesn't come back in 5s
if (conn.connect(stream, addr, &ct) != 0) return false;   // ← the handshake above
stream.send_n(request.data(), request.size());            // ← send the "GET ..." bytes
// ... recv() in a loop until the device closes the connection (75s cap) ...
```

`ACE_SOCK_Connector::connect()` **is** the three arrows at the top; `send_n()`
and `recv()` are the request/response bytes. There is no special "VPN API" in the
code — it's a plain socket, and the routing table + OpenVPN quietly do the
tunnelling underneath. That's the elegance: **the proxy writes ordinary TCP; the
tunnel makes an unreachable device reachable.**

### A.6 Where the cloud gets `10.9.0.7` from

The proxy is handed a URL like `/dev/urn%3Adev%3Agateway-42/api/v1/status`, not
an IP. It turns the endpoint name into the tunnel IP by reading the
`cloud.endpoints` list in the data store (`resolve_dev_tun_ip()`):

```
  /dev/<ep>/...   ──url-decode──►  ep = "urn:dev:gateway-42"
                                       │  look up in cloud.endpoints JSON
                                       ▼
                    { "endpoint": "urn:dev:gateway-42", "dev_tun_ip": "10.9.0.7", ... }
                                       │
                                       ▼
                          connect to 10.9.0.7 : cloud.proxy.device.ui.port (8080)
```

`dev_tun_ip` is written by `iot-cloudd` when the device's tunnel comes up, and
cleared when it drops. So:

- **no such endpoint** → nothing to connect to → **404**;
- **endpoint exists but `dev_tun_ip` is empty** (tunnel down) → **502
  "device tunnel is down"**;
- **tunnel up but httpd not answering** → connect/`recv` fails → **504**.

This lookup is also the security boundary (**SSRF-safe**): the cloud will only
ever `connect()` to an IP that it itself put in `cloud.endpoints`, on the one
fixed port `8080` — a malicious URL can never steer the proxy at an arbitrary
host or port.

### A.7 One more layer on top: the HTTP reverse proxy

§A.5 gets a raw TCP connection to the device's httpd. The remaining job — turning
the operator's browser request under `https://<cloud>/dev/<ep>/…` into the right
request to the device and fixing up the reply — is the **three rewrites** in §3
(path strip, `<base href>` inject, `Set-Cookie` Path). Those are pure HTTP text
edits layered *on top of* the tunnelled socket; they're covered above and in the
handler. The tunnel (this appendix) is the plumbing; the rewrites are what make a
single SPA work under a per-device URL prefix.

### A.8 The ports, all in one place

| Port | Where | Role |
|------|-------|------|
| `443` (or `80`) | cloud, public | The single origin the operator's browser talks to (`/webui/`, `/dev/<ep>/…`). |
| `1194` | cloud, public | OpenVPN — the **mouth of the pipe**; devices dial in here and hold it open. |
| `10.9.0.1` | cloud, inside tunnel | Cloud's `tun0` — the cloud end of every device's pipe. |
| `10.9.0.x` | device, inside tunnel | The device's `tun0` (`dev_tun_ip`) — what the proxy `connect()`s to. |
| `8080` | device | The device httpd (`http.listen.port`, bound `0.0.0.0`) — the actual UI server. |

**In one line:** the operator hits `https://<cloud>/dev/<ep>/`; `iot-httpd` looks
up the device's `10.9.0.x`, opens a plain TCP socket to `:8080` that OpenVPN
silently encrypts and tunnels to the NATed device, relays the HTTP with three
small rewrites, and streams the reply back — all over the one public origin, no
per-device port, no inbound hole in the device's router.
