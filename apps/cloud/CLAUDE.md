# apps/cloud — IoT Cloud Server

Multi-tenant IoT gateway platform. Runs on a cloud VM, manages N IoT
devices behind NAT via OpenVPN tunnels, serves as the single LwM2M
authority (Bootstrap + Device Management), reverse-proxies each
device's local UI, and serves the Cloud Operator Dashboard.

## Architecture

```
                    ┌──── lwm2m-bs (coaps://0.0.0.0:5684)
                    │
ds-server ── iot-cloudd ── iot-httpd (REST API + Cloud UI)
    │          │   │
    │          │   └──── lwm2m-dm (coaps://0.0.0.0:5683)
    │          │
    └──────────┴────── all IPC via /var/run/iot/data_store.sock
```

**Four binaries** (all in one image) → **five containers** at runtime:
- `ds-server` — shared data store (Lua-backed, schema-enforced)
- `iot-cloudd` — LwM2M BS/DM logic, VPN registry, endpoint provisioning
- `iot-httpd` — REST API (/api/v1/*) + serves Cloud UI (/webui/)
- `lwm2m` — **device binary reused in server role** for CoAP/DTLS transport;
  runs as two separate containers (`lwm2m-bs` on 5684, `lwm2m-dm` on 5683)

All daemons communicate exclusively through ds-server — no HTTP between
daemons. Same pattern as the device-side stack.

**Why reuse the device lwm2m binary?**  The device binary already has full
CoAP/UDP/DTLS support and handles `/bs` (bootstrap) and `/rd` (registration)
dispatch. Running it in `role=server` mode gives us the LwM2M CoAP endpoints
without writing a second CoAP stack. MongoDB linking is patched out (the
cloud doesn't need the device-side registration mirror).

**Daemon self-state:** Every daemon writes its own state to ds
immediately after connecting: iot-cloudd writes
`services.cloud.iot.cloudd.state` / `services.cloud.openvpn.server.state`,
iot-httpd writes `services.cloud.iot.httpd.state`, and the LwM2M CoAP
containers (lwm2m-bs, lwm2m-dm) run the lwm2m binary with `lwm2m-instance=bs|dm`
which self-reports `services.cloud.lwm2m.*.state` immediately after
connecting to ds. All state keys default to
`"stopped"` in the schema — the daemon self-reports `"running"` at startup
and `"exited"` at shutdown. The Services page polls these keys every 5s.

## Directory layout

```
apps/cloud/
├── CLAUDE.md           # this file
├── Dockerfile          # multi-stage build (Ubuntu builder + Node UI + slim runtime)
├── docker-compose.yml  # 5-service orchestration
├── run.sh              # podman/docker compose wrapper
├── server/             # iot-cloudd C++ source
│   └── src/main.cpp    # wires EndpointRegistry + VpnRegistry + BootstrapProvisioner
└── ui/                 # Angular 14 Cloud Operator Dashboard (Clarity)
    └── src/app/
        ├── dashboard/       # Online device count, tunnel status, telemetry
        ├── endpoint-list/   # Device table with Launch UI buttons
        ├── http-config/     # HTTP server config (reuses http.* schema)
        ├── vpn/             # OpenVPN server config + status
        ├── wan/             # WAN interface / WiFi
        ├── routing/         # Port forward / DNAT rules
        ├── lwm2m/
        │   ├── lwm2m-config/  # DM (Device Management) config
        │   ├── lwm2m-submenu/ # Shared subnav-bar (DM | Bootstrap Config)
        │   └── bs-config/     # BS (Bootstrap Config) + Provision Device
        ├── services/        # Service management (cloud daemon cards)
        └── log-level/       # Log viewer + log-level selector
```

## Cloud UI Nav

```
Dashboard  Endpoints  Map  VPN  HTTP  WAN  Routing  LwM2M         Services  Logs
                                                         ├─ Device Management
                                                         └─ Bootstrap Config
```

| Page | Writes to |
|------|-----------|
| Dashboard | reads /api/v1/status (long-poll) |
| Endpoints | reads /api/v1/cloud/endpoints; provision form → cloud.provision.bs.psk + cloud.provision.request; delete deprovisions. Endpoint name links to **Map** (focus) |
| Map | **Fleet Map** — client-side Leaflet, markers from `cloud.vehicle.telemetry` (live GPS + OBD-II popups); tiles from the self-hosted `tileserver` compose service. See `apps/docs/tdd-vehicle-telemetry.md` |
| VPN | cloud.vpn.* |
| HTTP | http.* (reused from device) + http.auth.enabled |
| WAN | vpn.* / wifi.* / net.* (copied from device UI) |
| Routing | net.* (port forward / DNAT) |
| LwM2M → Device Management | cloud.dm.* |
| LwM2M → Bootstrap Config | cloud.bs.* + provision → cloud.provision.* |
| Services | services.cloud.* (polled every 5s) |
| Logs | log.level + GET /api/v1/log |

### Debug mode (developer aid)

A global **Debug** toggle sits at the bottom of the sidebar next to
**Dark** (in both cloud-ui and iot-ui). Like the theme toggle it is a
`providedIn:'root'` `DebugService` persisted per-browser in
`localStorage['iot-debug']` — no backend state.

When on, every config form input shows — just below it — the data-store
key that fills the field plus an **editable raw value box** bound straight
to that key, so an operator building their own app can discover and poke
the ds key/value behind any field. Implementation (in each UI's
`src/common/`):

- `debug.service.ts` — the on/off flag.
- `ds-debug.directive.ts` — `*dsDebug`, a structural directive that renders
  its content only while Debug is on (placed on `<clr-control-helper>` so
  the hint slots into Clarity's below-input region and keeps the 4-column
  `.form-grid` aligned).
- `ds-hint.component.ts` — `<app-ds-hint key="cloud.vpn.subnet">`, which
  self-reads/writes the raw value via `/api/v1/db`, admin-gated.

Wire a new field with one line — no component changes needed:
```html
<clr-control-helper *dsDebug><app-ds-hint key="cloud.vpn.subnet"></app-ds-hint></clr-control-helper>
```
Array/datagrid editors (no single key) and checkbox containers are not
wired.

## API patterns

All config pages use the same REST surface.  The UI always calls the
same-origin iot-httpd (no CORS):

| Operation | Method | Path | Payload |
|-----------|--------|------|---------|
| Read keys | POST | /api/v1/db/get | `{keys: ["a.b", ...]}` |
| Write keys | POST | /api/v1/db/set | `{pairs: [{key, value}, ...]}` |
| Long-poll | GET | /api/v1/db/get?key=k&timeout=N | query params |
| Login | POST | /api/v1/auth/login | `{id, password}` |
| Logout | POST | /api/v1/auth/logout | — |
| Status | GET | /api/v1/status?timeout=N | — |
| Cloud endpoints | GET | /api/v1/cloud/endpoints | — (ds-backed) |
| Log text | GET | /api/v1/log | — (text/plain) |

All `/api/v1/*` routes except `/api/v1/auth/*` require a session cookie.

## Naming convention

All ds keys use **dots only** — no hyphens, no underscores:
```
cloud.bs.psk.id          ✓
cloud.bs.psk_id          ✗ (underscore)
cloud.bs.psk-id          ✗ (hyphen)
services.cloud.iot.cloudd.enable   ✓
services.cloud.iot-cloudd.enable   ✗ (hyphen)
```

## Building

The cloud image builds four binaries from the same source tree:

| Binary | Source | Flags |
|--------|--------|-------|
| `ds-server`, `ds-cli` | `modules/data-store/` | — |
| `iot-httpd` | `modules/http-server/` | — |
| `iot-cloudd` | `apps/cloud/server/` | — |
| `lwm2m` | `apps/CMakeLists.txt` | `-DIOT_ENABLE_MONGO=OFF` |

The `lwm2m` binary is the **same** one built by `docker/Dockerfile` for
device images — only the `IOT_ENABLE_MONGO` flag differs (device = ON,
cloud = OFF).

```bash
# From repo root:
podman build -t naushada/iot-cloud:latest -f apps/cloud/Dockerfile .

# Or use run.sh:
cd apps/cloud && ./run.sh build
```

## Running

```bash
cd apps/cloud
./run.sh           # docker compose up -d (all 5 services)
./run.sh logs      # tail logs (ds-cli + service states)
./run.sh stop      # stop all
```

### Volume persistence and schema updates

`docker-compose.yml` uses named volumes so data survives container
restarts **and host reboots** (named volumes are on-disk under the runtime's
`volumes/` store; only an explicit `volume rm` destroys them):

| Volume | Mount point | Content |
|--------|-------------|---------|
| `iot-etc` | `/etc/iot` | Schemas, config files |
| `iot-lib` | `/var/lib/iot` | Persisted data store (`data_store.lua`) |
| `iot-run` | `/var/run/iot` | Unix socket (tmpfs-like) |

The data store at **`/var/lib/iot/data_store.lua`** is write-through +
`fsync` on every `set`, so committed config/credentials persist across a
reboot. Host path: `$(docker volume inspect cloud_iot-lib --format '{{.Mountpoint}}')/data_store.lua`.
See `modules/data-store/docs/design.md` §4.4 for the full persistence/host-path
matrix.

**Important:** Named volumes are populated from the image only on
**first creation**. After a rebuild that changes schema files
(`services.lua`, `iot.lua`, etc.) or config files under `apps/config/`,
the old volume still has the old files. To pick up changes:

```bash
./run.sh stop
podman volume rm cloud_iot-etc   # or: docker volume rm cloud_iot-etc
./run.sh
```

The volume will be recreated with the latest image contents.
`iot-lib` (persisted data) and `iot-run` (socket) do not need to be
reset.

Service ports:
| Port | Service |
|------|---------|
| 80 | Cloud UI + REST API |
| 5683 | LwM2M DM (CoAPs) |
| 5684 | LwM2M Bootstrap (CoAPs) |
| 1194 | OpenVPN (UDP) |

## LwM2M CoAP Server

The device `lwm2m` binary (from `apps/CMakeLists.txt`) is built inside the
cloud image and run as two containers in `role=server` mode:

| Container | Port | Role | CoAP Endpoint |
|-----------|------|------|---------------|
| `lwm2m-bs` | 5684/udp | Bootstrap Server | `coaps://0.0.0.0:5684` |
| `lwm2m-dm` | 5683/udp | Device Management | `coaps://0.0.0.0:5683` |

**PSKs are never hardcoded.** Neither server registers a shared/default
identity or secret — there is no `LWM2M_PSK_ID/KEY` env or CLI arg in the
deployed compose. Each server authenticates a handshake against the
**per-endpoint credential** for the identity the client presents, looked up
**live from `cloud.endpoint.credentials`** by a ds-backed PSK resolver
(`DTLSAdapter::set_psk_resolver`, wired in `apps/src/main.cpp`):

- **BS** matches `sha256(serial)[:32] == presented-identity` → `bs.psk.key`;
  if that misses, falls back to matching the row's `identity` / `dm.psk.id`
  (the formatted `rpi<serial>@cloud.local`) → the same `bs.psk.key`. The
  fallback covers devices that present their DM-style identity at the BS
  handshake (`iot.bs.psk.override=true`); without it, a cloud reboot's
  re-bootstrap would wedge such a device offline (see `troubleshoot.md`).
- **DM** matches `dm.psk.id == presented-identity` → `dm.psk.key`

The resolver runs on the handshake/reactor thread (not the ds listener
thread), so its blocking `get()` is safe; reading live means a device
provisioned *after* the server started authenticates with no restart, and ds
stays the single source of truth. The identity itself is always *derived from
the serial* (BS `sha256`, DM `rpi<serial>@cloud.local`), never stored. A
`identity=/secret=` CLI override still exists but only for the dev/interop
test harness — production passes nothing.

**Zero-touch HKDF tier (added 2026-06-26, `apps/docs/tdd-bs-hkdf-zerotouch.md`).**
The resolver above is the **commissioned** tier (a `cloud.endpoint.credentials`
row per device). When no row matches AND a master is configured, the server
**HKDF-derives** the PSK instead — `resolve_bs_psk` / `resolve_dm_psk`
(`apps/src/provisioning_policy.cpp`):
- **BS**: device presents its **raw serial** (override path) → `HKDF(master, "iot-bs-psk:v1:"+serial)`.
- **DM**: presents `rpi<serial>@cloud.local` → `HKDF(master, "iot-dm-psk:v1:"+serial)`.

The master lives in `cloud.bs.master.key` AES-256-GCM-**wrapped**; a KEK
(`IOT_BS_MASTER_KEK` / `IOT_BS_MASTER_KEK_FILE` / systemd `$CREDENTIALS_DIRECTORY/bs_kek`,
**never** in ds) unwraps it once at startup into memory. No KEK / bad blob →
fail closed to the commissioned tier (the HKDF tier is off by default). The
provisioning resolver likewise derives the BS+DM accounts for an un-stored
serial — fully stateless, nothing minted or stored. Devices are flash-time
personalised with `iot-bs-personalize`; the wrap tool is `bs-master-wrap`.
Tiers **coexist**: stored row wins, else derive.

The device binary already handles `/bs` (bootstrap session) and `/rd`
(registration) dispatch over CoAP/UDP/DTLS. It is built from the same
`apps/CMakeLists.txt` as the device, with `-DIOT_ENABLE_MONGO=OFF` to
skip MongoDB linking (the cloud doesn't need the device-side registration
mirror).

**Device schemas and configs** (`net.lua`, `vpn.lua`, `wifi.lua`, and
`apps/config/`) are also copied into the cloud image at `/etc/iot/` so
the lwm2m binary finds the schema and provisioning data it expects.

## Device UI over VPN

Each registered device runs its own web UI behind NAT, reachable from the
operator only through the tunnel. The cloud exposes it via a per-device DNAT.

**Per-device mapping.** When a device registers it is assigned a VPN virtual
IP (`tun_ip`, e.g. `10.9.0.x`) and a proxy port (`proxy_port`, allocated by
`VpnRegistry`); both live in the `cloud.endpoints` ds JSON. Operator access is
`http://<cloud-host>:<proxy_port>/` — the Endpoints page **Launch UI** link —
which DNATs to the device's UI over the tunnel.

**nftables DNAT (`modules/server/dnat`).** `iot-cloudd` installs a per-device
DNAT for every endpoint that has both a `tun_ip` and a `proxy_port`:

```
tcp dport <proxy_port> dnat to <tun_ip>:<ui_port>     # prerouting (nat)
oifname "tun0" masquerade                              # postrouting (nat)
oifname/iifname "tun0" accept                          # forward (filter)
```

The whole ruleset lives in its own table `ip iot_cloud_dnat` so re-applying
flushes only our rules (never NetworkManager / docker / openvpn chains). It is
built (`build_device_dnat_ruleset`) from `cloud.endpoints` and applied with
`nft -f -` (`apply_ruleset`) at startup and on every provision / deprovision /
registration change — idempotent (full-table flush + rebuild). `iot-cloudd`
also enables IPv4 forwarding (`enable_ip_forward` → `net.ipv4.ip_forward=1`)
once at startup.

**ds-driven proxy ports:**

| Key | Default | Meaning |
|-----|---------|---------|
| `cloud.proxy.device.ui.port` | `80` | the device's web-UI port (DNAT target port); seeded at startup |
| `cloud.vpn.proxy.port.start` | `10000` | low end of the proxy-port range `VpnRegistry` allocates from |
| `cloud.vpn.proxy.port.end` | `10050` | high end of the proxy-port range |

The range is read from ds at startup (CLI `proxy-start=`/`proxy-end=` are
fallbacks) and written back so it is visible/editable; changing
`cloud.proxy.device.ui.port` triggers a DNAT rebuild.

**Networking: bridge + published ports** (NOT host networking). `iot-cloudd`
runs on the default bridge and **publishes** `1194/tcp` + the proxy range
(`PROXY_START-PROXY_END`, default `10000-10050`) via docker-compose `ports:`.
Published ports bypass the host `ufw` (the `DOCKER-USER` chain), so they're
reachable without per-port ufw rules — host networking was tried and reverted
because it subjected `1194` to ufw (allow-22-only → connection refused). Notes:

- The proxy range is kept **small and above the CoAP ports** (5683/5684):
  each published port spawns a `docker-proxy` process, so a wide range (the
  old `5001-6000` = 1000 ports) exhausts the host. Widen `PROXY_START/END`
  for a bigger fleet, or switch to host networking (no per-port proxy).
- The **published** range (`ports:`, a deploy-time env knob Docker reads
  before ds exists) must cover the **ds** allocation range
  (`cloud.vpn.proxy.port.*`); `run.sh` defaults both to `10000-10050`.
- `iot-cloudd` has `NET_ADMIN` + `sysctls: net.ipv4.ip_forward=1` in compose
  to run the OpenVPN server tun + install the per-device DNAT.

The cloud also pushes the VPN *endpoint* (host/port/proto) down to the device
over LwM2M Object 2048 — see `apps/docs/lwm2m-object-handling.md` §2.8. The VPN
**host** is additionally *derived on the device* from the bootstrap-delivered
DM URI (the VPN concentrator is co-located with the DM), so a co-located cloud
needs no VPN-host config on the device; the Object-2048 push is the override for
a split topology (derive-with-override).

## Log Levels

Default is `INFO` for all daemons. Per-daemon keys override the global
`log.level`; an empty per-daemon value means "inherit global".

| Key | Scope |
|-----|-------|
| `log.level` | Global default (default `"INFO"`) |
| `log.level.cloudd` | iot-cloudd |
| `log.level.httpd` | iot-httpd |
| `log.level.lwm2m.bs` | lwm2m Bootstrap Server |
| `log.level.lwm2m.dm` | lwm2m Device Management |
| `log.level.vpn` | VPN server |
| `log.level.dtls` | DTLS stack |

Valid levels: `DEBUG`, `INFO`, `WARNING`, `ERROR`.

Changes take effect immediately — each daemon watches its log level key
and hot-reloads via `ACE_Log_Msg::priority_mask()`.

### Enabling DEBUG at boot

On a cold first boot the schema default `"INFO"` wins. To start at DEBUG:

**Persist the value (survives restarts):**
```bash
ds-cli set log.level DEBUG
```
Once set via the cloud UI or ds-cli, the value is persisted to
`/var/lib/iot/data_store.lua` and loads before any daemon connects on
the next boot.

**Per-daemon at boot (before the daemon starts):**
```bash
ds-cli set log.level.cloudd DEBUG   # cloudd only
```

Daemons self-report their log output to ds so the cloud UI log viewer
can tail them live:

| Daemon | Log key |
|--------|---------|
| iot-httpd | `log.text` |
| iot-cloudd | `log.cloudd.text` |
| lwm2m (client) | `log.lwm2m.text` |
| lwm2m-bs | `log.lwm2m.bs.text` |
| lwm2m-dm | `log.lwm2m.dm.text` |

Each log line includes the daemon name for debugging: `... cloudd: ...`,
`... httpd: ...`, `... lwm2m: ...`.

## Auth

Session-auth via cookies (SHA-256 password hashing). Default: `admin/admin`.
Auth is enabled by default (`http.auth.enabled=true` in ds). Non-API paths
(/webui/*, /index.html, etc.) are public so the login page loads before auth.
All `/api/v1/*` routes except `/api/v1/auth/*` require a valid session.

Disable auth at runtime:
```bash
ds-cli set http.auth.enabled false
```

## Key data store paths

### HTTP Server (http.*) — reused from device schema
```
http.listen.ip           → Bind address (default "0.0.0.0")
http.listen.port         → Listen port (default 8080)
http.listen.scheme       → "http" | "https"
http.tls.cert            → TLS cert PEM path (required for https)
http.tls.key             → TLS key PEM path (required for https)
http.tls.ca              → CA bundle PEM path (optional — enables mTLS)
http.workers             → Handler thread pool (0 = inline; schema default 4 —
                           enough for the cloud-ui long-polls). ds-ONLY: no CLI
                           arg / env — the compose used to pass http-workers=
                           which silently overrode this key, so the UI showed 0
                           while the daemon ran 4. Now ds is the single source of
                           truth (set it in the HTTP page).
http.auth.enabled        → Auth gate (true = enabled, from auth.lua)
```
Hot-reloaded: all except `http.workers` — a change there makes iot-httpd
**self-restart** to resize the pool (cloud restarts via compose
`restart: unless-stopped`; device unit uses `Restart=always`).

### Bootstrap Server (cloud.bs.*)
```
cloud.bs.endpoint        → BS server identity (default: urn:dev:gateway-)
cloud.bs.uri             → BS CoAPs endpoint (default: coaps://0.0.0.0:5684)
cloud.bs.security.mode   → "PSK" | "None"
cloud.bs.psk.id          → BS PSK identity (RID 3)
cloud.bs.psk.key         → BS PSK secret (RID 5, opaque)
cloud.bs.master.key      → zero-touch HKDF master, AES-256-GCM-wrapped (opaque,
                           write-only gid:cloud-svc; "" = HKDF tier off). KEK is
                           out-of-band, never in ds. See tdd-bs-hkdf-zerotouch.md.
```

### Device Management (cloud.dm.*)
```
cloud.dm.uri             → DM server URI (pushed to devices, RID 0)
cloud.dm.lifetime        → Default registration lifetime (RID 1, default 90 — NAT keepalive, see below)
cloud.dm.binding         → Default binding mode (RID 7, default "U")
cloud.dm.psk.id          → DM PSK identity (post-bootstrap)
cloud.dm.psk.key         → DM PSK key (post-bootstrap, opaque)
cloud.dm.lwm2m.version   → LwM2M version (default "1.1")
```

#### Registration lifetime & NAT keepalive

The LwM2M control plane is **direct device→cloud DTLS over UDP (:5683)** — the
device is behind the home-router / ISP **NAT**, whose UDP conntrack mapping
expires after a short idle. If it expires, the cloud can no longer reach the
device (OTA push, server Reads) until the device speaks again. So the
registration **Update doubles as the NAT keepalive**: the device sends one at
`lifetime − 30s` (the fixed `updateMarginSeconds`), and that traffic keeps the
mapping alive.

`cloud.dm.lifetime` is therefore sized to the NAT timeout, **not** left at a
day. netfilter conntrack defaults (what most gateways are built on):

| Flow | conntrack timeout | sysctl |
|------|-------------------|--------|
| UDP, unreplied | **30 s** | `nf_conntrack_udp_timeout` |
| UDP, assured/stream | **120 s** | `nf_conntrack_udp_timeout_stream` |
| TCP, established | **432000 s (5 d)** | `nf_conntrack_tcp_timeout_established` |

- **Default `lifetime = 90`** → Update every **60 s** ≈ half the 120 s
  assured-UDP timeout (classic keepalive = timeout/2). Also covers most CGNATs.
- **Aggressive CGNAT (30–60 s UDP):** lower to `60` (→30 s Update).
- The **VPN tunnel** is openvpn over **TCP** + `ping 10` (10 s), so its mapping
  never idles out — it needs no tuning. The 24 h default only makes sense if
  LwM2M itself is **routed over the tunnel** (then the tunnel's keepalive covers
  it and `lifetime` can go back to 86400). Until that's done, keep it NAT-sized.

A lost registration (e.g. cloud `lwm2m-dm` restart) is *also* recovered fast by
the **VPN-reconnect-triggered re-Register** in the client (`apps/src/main.cpp`,
`vpn.state` watch) — complementary to this keepalive.

### Provision (per-device)
```
cloud.provision.request   → Endpoint name (serial) to provision (iot-cloudd watches)
cloud.deprovision.request → Endpoint name to deprovision (iot-cloudd watches)
```
Provisioning is driven from the cloud-ui **Endpoints** page via the PSK flow
below (serial + BS PSK → `cloud.endpoint.credentials`). The Security/Server
object TLVs pushed at `/bs` are built by the `lwm2m-bs` server from
`cloud.bs.*` / `cloud.dm.*` + the per-endpoint `cloud.endpoint.credentials`
(see below) — there is no per-endpoint `cloud.provision.configs` override.

### PSK provisioning (serial-derived endpoint + write-only PSK)

See `apps/docs/tdd-psk-provisioning.md`. Each device's serial is the LwM2M
endpoint and the on-the-wire BS DTLS PSK identity (raw serial). The cloud
owns the formatted identity `rpi<serial>@cloud.local`, which keys both the
BS and DM PSKs. The **BS PSK is generated in device-ui** (browser) and pasted
into the cloud Endpoints page; the **DM PSK is minted by iot-cloudd** per
endpoint. Provisioning is **db/set-driven** — no bespoke REST endpoint.

Flow: cloud-ui writes `cloud.provision.bs.psk` (carrier) then
`cloud.provision.request` = serial (the existing watched trigger);
iot-cloudd formats the identity, mints the DM PSK, upserts
`cloud.endpoint.credentials`, and clears the carrier.

```
cloud.provision.bs.psk    → Carrier: engineer-pasted BS PSK (64 hex).
                            Cleared by iot-cloudd after upsert. (gid:cloud-svc)
cloud.endpoint.credentials → JSON array, per-endpoint creds. The BS/DM
                            servers load this live (BS keys by raw serial,
                            DM by formatted identity). Write-only /
                            no ds-cli read. (gid:cloud-svc)
  [{
    "serial":     "100000abcd",
    "identity":   "rpi100000abcd@cloud.local",
    "bs.psk.key": "<64 hex>",
    "dm.psk.id":  "rpi100000abcd@cloud.local",
    "dm.psk.key": "<64 hex>"
  }]
cloud.dev.mode            → Commissioning flag. While true the ds-server
                            bypasses the PSK ACLs so cloud-httpd can write
                            the carrier + reveal creds. (gid:cloud-svc)
```

Device-side PSK keys (`iot.lua`, loaded by the same ds-server; client runs
as `engineer`):
```
iot.serial                → Hardware serial. RPi: auto-filled by the client
                            at startup; non-RPi: installer enters via
                            device-ui. = endpoint = BS PSK identity. (gid:engineer write)
iot.dev.mode              → Commissioning flag (bypasses PSK ACLs). (gid:engineer)
iot.bs.psk.identity       → = raw serial (on-the-wire BS identity). (gid:engineer rw)
iot.bs.psk.key            → BS PSK, hex. device-ui-generated, write-only. (gid:engineer rw)
iot.dm.psk.identity       → = rpi<serial>@cloud.local, from bootstrap. (gid:engineer rw)
iot.dm.psk.key            → DM PSK, hex, from bootstrap, write-only. (gid:engineer rw)
```
`gid:engineer`/`gid:cloud-svc` are unix groups (the static service accounts
the client / cloud servers run as) — not ds keys, so hyphens are allowed.

### Endpoints (written by iot-cloudd, read by cloud UI)

`EndpointRegistry` + `VpnRegistry` are in-memory, but iot-cloudd **rehydrates
them from ds at startup** (`rehydrate_registry`, before the first sync): it
exact-restores `tun_ip`/`proxy_port`/registered from the persisted
`cloud.endpoints`, and heals any provisioned endpoint in
`cloud.endpoint.credentials` that lacks a registry row. Without this the empty
registry would overwrite `cloud.endpoints` with `[]` on every restart — a
provisioned device would vanish from the table and lose its tun_ip/proxy_port.
PSKs are never touched (only the registry/allocations are rebuilt).

```
cloud.endpoints          → JSON array of provisioned endpoints
  [{
    "endpoint":      "urn:dev:gateway-42",
    "state":         "online",
    "tun_ip":        "10.9.0.12",
    "proxy_port":    10000,
    "registered":    true,
    "last_seen_unix": 1718123456,
    "isp_ip":        "65.49.1.75",     // device public/ISP IP (openvpn real-addr)
    "lan_ip":        "192.168.1.3"     // device LAN IP on its active WAN iface
  }]
cloud.lwm2m.registrations → JSON array of currently-registered endpoints.
                            SOLE writer = lwm2m-dm (from its ClientRegistry on
                            Register/Update/Deregister + lifetime expiry).
                            iot-cloudd watches it and merges online/offline +
                            last_seen into cloud.endpoints (separate key avoids
                            a two-writer clobber on tun_ip/proxy_port).
  [{ "endpoint": "100000abcd", "registered": true, "last_seen_unix": 1718123456,
     "installed_version": "1.2.0", "lan_ip": "192.168.1.3" }]
cloud.vehicle.telemetry  → live vehicle telemetry per endpoint, JSON array.
                            SOLE writer = lwm2m-dm (server-Reads device Object 6
                            GPS + Object 33000 OBD-II via the token-tagged poll —
                            see apps/docs/lwm2m-object-handling.md §5). VOLATILE,
                            latest-wins. The cloud-ui Fleet Map reads it live.
  [{ "endpoint": "100000abcd", "lat": "12.97", "lon": "77.59",
     "speed": "62", "rpm": "2150", "coolant": "89", "throttle": "18",
     "load": "34", "fuel": "71", "iat": "31", "maf": "5.2",
     "link": "up", "dtc": "[]" }]
```
The 60-day vehicle history (Mongo + archiver) is a separate, opt-in pipeline —
see the `mongo`/`tileserver`/`iot-archiver` compose services (`profiles:
[telemetry]`) and `apps/docs/tdd-vehicle-telemetry.md`.

**Two device IP columns (`isp_ip` / `lan_ip`).** The Endpoints table shows both
the device's public IP and its local IP — sourced from *different* planes:

- **`isp_ip`** (public/ISP) comes from the **OpenVPN management plane**:
  `iot-cloudd`'s `poll_vpn_client_ips` parses the server `status` ROUTING TABLE
  (`<virtual-ip>,<CN>,<real-addr>,<ref>`); the `real-addr` is the NAT public IP
  the tunnel arrives from. No device cooperation needed; empty when the tunnel
  is down.
- **`lan_ip`** (local) comes from the **LwM2M plane**: `lwm2m-dm` server-reads
  the device's `/4/0/4` (Connectivity Monitoring → IP Addresses) over the
  tunnel; the device serves it from `net.iface.active.ip`, which **net-router**
  publishes for whichever WAN iface (eth0/wlan0/wwan0) it currently routes
  through (see `modules/net/router/docs/design.md`). lwm2m-dm writes it into
  `cloud.lwm2m.registrations`; `iot-cloudd` merges it into `cloud.endpoints`
  (`update_lan_ip`) and rehydrate restores it. Empty until the first read.

Same merge discipline as `installed_version` (/3/0/3): lwm2m-dm is the sole
writer of `cloud.lwm2m.registrations`, iot-cloudd merges into `cloud.endpoints`
— so the device-reported facts never two-writer-clobber the registry's
`tun_ip`/`proxy_port`.

### VPN Server (cloud.vpn.*)
```
cloud.vpn.subnet         → Tunnel subnet (default 10.9.0.0/24); ds-only (no
                           VPN_SUBNET env/CLI — read live by iot-cloudd)
cloud.vpn.port.next      → Next proxy port (10000–10050)
cloud.vpn.ca.crt         → CA cert path
cloud.vpn.ca.key         → CA key path (secret volume)
cloud.vpn.server.crt     → Server cert path
cloud.vpn.server.key     → Server key path
```

### VPN PKI & per-device certificates (centralized minting)

**The cloud is the sole PKI authority — it mints _everything_, including each
device's client private key.** There is no device-side key generation or CSR.
`CertAuthority` (`modules/server/openvpn/`) runs entirely on iot-cloudd:

- **Runtime CA** — `ensure()` generates (or restores from ds) the CA key + cert
  at `/etc/iot/vpn/ca/{ca.key,ca.crt}`. The **CA private key never leaves the
  cloud**. The CA DB (`index.txt`, `serial`, CRL) is scaffolded by `ensure_crl()`
  on every startup (`unique_subject = no`, so re-minting the same CN is allowed).
- **Server cert** — minted once for the openvpn server (`server.crt/key`).
- **Per-device client cert+key** — on `cloud.provision.request`,
  `mint_client(cn)` runs **`openssl genrsa` (client key) → `req` (CSR) →
  `ca` (CA-sign)** in a temp dir and returns a `MintedCert{client_crt,
  client_key, ca_crt}`. `cn` = `rpi<serial>@cloud.local`.

**Delivery (cloud → device):** the minted material lands in
`cloud.endpoint.credentials` (`vpn.client.cert`, `vpn.client.key`) + the CA in
`cloud.vpn.ca.crt.pem`; the DM server then **pushes the cert family over LwM2M
custom Object 2048** (instances 0/1/2 = ca/cert/key) and EXECUTEs RID 3 (Apply).
On the device, `install_cert` (`apps/src/lwm2m_object_stubs.cpp`) materialises
`/etc/iot/vpn/{ca.crt,client.crt,client.key}` (dir `2750 engineer:iot`; the **key
`0640` group `iot`** so the openvpn-client DynamicUser via
`SupplementaryGroups=iot` can read it; the certs `0644`), then a
`services.openvpn.client.enable` `false→true` gate-flip respawns openvpn-client
to load them. See `modules/openvpn/client/docs/design.md` §"cert-arrival respawn".

```
cloud:  genrsa+req+ca (CertAuthority)  ──►  cloud.endpoint.credentials
                                            {vpn.client.cert, vpn.client.key}
                                            cloud.vpn.ca.crt.pem
            │  LwM2M Object 2048 PUT(ca/cert/key) + EXECUTE Apply
            │  (over the device's DTLS-PSK DM session)
            ▼
device: /etc/iot/vpn/{ca.crt, client.crt, client.key}  ──►  openvpn-client
```

#### Security considerations

- **Client private key is generated on the cloud and transits the wire.** Unlike
  a device-generated-key + cloud-signs-CSR model (where the key never leaves the
  device), here the key is created on iot-cloudd, **stored in ds**
  (`cloud.endpoint.credentials` — write-only: `gid:cloud-svc`, no `ds-cli` read,
  same ACL as the PSKs), **pushed over LwM2M Object 2048 on the DTLS-PSK DM
  session**, and stored on the device at `/etc/iot/vpn/client.key` (`0640`, group
  `iot`). It is protected in transit by DTLS and at rest by ds ACLs + filesystem
  perms, but it **does exist in three places** (cloud ds, the wire, device disk).
  Trade-off accepted
  for simplicity (no openssl/keygen on the RPi). **Future hardening:**
  device-generated key + cloud-signs-CSR so the private key never leaves the
  device — would also remove the key from ds and the control plane.
- **CA private key is cloud-only** (`/etc/iot/vpn/ca/ca.key`, restored from ds);
  it is never delivered to any device. A device only ever gets the CA **cert**.
- **Revocation** — `revoke()` (`openssl ca -revoke` → CRL → `cloud.vpn.crl.pem`)
  runs on deprovision/transfer; openvpn enforces `crl-verify`. See
  `apps/docs/tdd-device-transfer.md`.
- **Re-provisioning a reflashed device** reuses the same CN
  (`rpi<serial>@cloud.local`, serial-derived) and re-mints — `unique_subject =
  no` permits it; a stale CA DB (`index.txt`/`serial`) is the failure mode to
  watch (the `openssl ca` step), now surfaced in the log (PR #434).
- **CA/cert expiry — no rotation today (known gap).** The CA cert, server cert,
  and every client cert are issued with `days = 3650` (**10 years**), and there is
  **no renewal/rotation or near-expiry detection**: `ensure()` early-returns
  whenever `ca.key` exists, so it reuses the CA forever and never regenerates. At
  expiry the whole VPN trust chain breaks (all certs untrusted → openvpn TLS
  handshakes fail → every tunnel down). **Saving grace:** the LwM2M control plane
  is **DTLS-PSK, not cert-based** (per-endpoint PSKs from
  `cloud.endpoint.credentials`), so it **survives CA expiry** — new certs are
  still deliverable **in-band** (no physical access). Recovery today is **manual**:
  remove `/etc/iot/vpn/ca/{ca.key,ca.crt}` + `index.txt`/`serial` so `ensure()`
  rebuilds a fresh CA + server cert, then **re-provision every device** to re-mint
  client certs; the per-tick Object-2048 push then redelivers the new CA cert +
  client cert over the surviving PSK channel and tunnels recover. **Future
  hardening:** detect near-expiry and auto-rotate with **overlapping validity**
  (push the new CA cert to devices *before* cutover so trust isn't lost
  mid-rotation), plus shorter-lived client certs on a periodic re-mint. The
  PSK-based (cert-independent) control plane is what makes remote rotation
  possible at all — a deliberate property.

### Services (services.cloud.*)
```
services.ds.state                           → ds-server state (default "stopped")
services.cloud.iot.cloudd.enable|state      → iot-cloudd enable/state
services.cloud.iot.httpd.enable|state       → iot-httpd enable/state
services.cloud.openvpn.server.enable|state  → openvpn-server enable/state
services.cloud.lwm2m.bs.state               → lwm2m-bs state (always-on, docker-compose managed)
services.cloud.lwm2m.dm.state               → lwm2m-dm state (always-on, docker-compose managed)
```
All state keys default to `"stopped"`. Each daemon self-reports `"running"`
immediately after connecting to ds-server, and `"exited"` at shutdown.
The Services page polls every 5s.

**ds-server state + L22 telemetry stream live in `/status`.** `iot-httpd`
dual-emits the `services.ds.*` keys (state + cpu/mem/fd/threads) into the flat
`cloud` passthrough the cloud-ui Services page reads. Without that they only
landed in the nested `services` block (which the *device*-ui reads) and never in
the `cloud` block (which only matched `services.cloud.*`), so the cloud
ds-server row showed a stale `"stopped"` + `—` telemetry even though it was
running — which it always is, since every other daemon reports *through* it.
The flat key for ds-server state is `services.ds.state` (the other services use
`services.cloud.*`).

## Related modules

| Module | Role |
|--------|------|
| `modules/data-store` | ds-server + client lib (IPC backbone) |
| `modules/http-server` | iot-httpd — reused for both cloud and device |
| `apps/CMakeLists.txt` | Device lwm2m binary (reused in server role) |
| `modules/server/lwm2m` | LwM2M BS/DM server libs (used by iot-cloudd) |
| `modules/server/openvpn` | VPN registry lib |
| `modules/server/web` | Device UI reverse proxy |

## Schemas

All schemas are under `modules/*/schemas/` and copied to `/etc/iot/ds-schemas/`:

| Schema | Scope |
|--------|-------|
| `iot.lua` | Device-side keys (log.level, log.text, iot.*) |
| `cloud.lua` | Cloud keys (cloud.bs.*, cloud.dm.*, cloud.vpn.*, cloud.provision.*) |
| `http.lua` | HTTP server config (reused cloud + device) |
| `auth.lua` | Auth config (http.auth.enabled) |
| `services.lua` | Service enable/state (services.* + services.cloud.*) |
| `vpn.lua` | VPN client keys (device-side) |
| `wifi.lua` | WiFi client keys (device-side) |
| `net.lua` | Net/router keys (device-side, reused for cloud DNAT) |
