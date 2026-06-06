# apps/cloud — IoT Cloud Server

Multi-tenant IoT gateway platform. Runs on a cloud VM, manages N IoT
devices behind NAT via OpenVPN tunnels, serves as the single LwM2M
authority (Bootstrap + Device Management), reverse-proxies each
device's local UI, and serves the Cloud Operator Dashboard.

## Architecture

```
ds-server ──→ iot-cloudd ──→ iot-httpd (REST API + Cloud UI)
    │              │
    └──────────────┴── all IPC via /var/run/iot/data_store.sock
```

**Three binaries** (all in one image):
- `ds-server` — shared data store (Lua-backed, schema-enforced)
- `iot-cloudd` — LwM2M BS/DM, VPN registry, endpoint provisioning
- `iot-httpd` — REST API (/api/v1/*) + serves Cloud UI (/webui/)

All daemons communicate exclusively through ds-server — no HTTP between
daemons. Same pattern as the device-side stack.

**Daemon self-state:** iot-httpd and iot-cloudd write
`services.cloud.iot.httpd.state` / `services.cloud.iot.cloudd.state`
to ds at startup ("running") and shutdown ("exited"). The Services page
polls these keys every 5s.

## Directory layout

```
apps/cloud/
├── CLAUDE.md           # this file
├── Dockerfile          # multi-stage build (Ubuntu builder + Node UI + slim runtime)
├── docker-compose.yml  # 3-service orchestration
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
Dashboard  Endpoints  VPN  HTTP  WAN  Routing  LwM2M              Services  Logs
                                                      ├─ Device Management
                                                      └─ Bootstrap Config
```

| Page | Writes to |
|------|-----------|
| Dashboard | reads /api/v1/status (long-poll) |
| Endpoints | reads /api/v1/cloud/endpoints, delete deprovisions |
| VPN | cloud.vpn.* |
| HTTP | http.* (reused from device) + http.auth.enabled |
| WAN | vpn.* / wifi.* / net.* (copied from device UI) |
| Routing | net.* (port forward / DNAT) |
| LwM2M → Device Management | cloud.dm.* |
| LwM2M → Bootstrap Config | cloud.bs.* + provision → cloud.provision.* |
| Services | services.cloud.* (polled every 5s) |
| Logs | log.level + GET /api/v1/log |

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

```bash
# From repo root:
podman build -t naushada/iot-cloud:latest -f apps/cloud/Dockerfile .

# Or use run.sh:
cd apps/cloud && ./run.sh build
```

## Running

```bash
cd apps/cloud
./run.sh           # docker compose up -d (all 3 services)
./run.sh logs      # tail logs
./run.sh stop      # stop all
```

Service ports:
| Port | Service |
|------|---------|
| 8080 | Cloud UI + REST API |
| 5683 | LwM2M DM (CoAPs) |
| 5684 | LwM2M Bootstrap (CoAPs) |
| 1194 | OpenVPN (UDP) |

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
http.workers             → Handler thread pool (0 = inline, default)
http.auth.enabled        → Auth gate (true = enabled, from auth.lua)
```
Hot-reloaded: all except `http.workers`.

### Bootstrap Server (cloud.bs.*)
```
cloud.bs.endpoint        → BS server identity (default: urn:dev:gateway-)
cloud.bs.uri             → BS CoAPs endpoint (default: coaps://0.0.0.0:5684)
cloud.bs.security.mode   → "PSK" | "None"
cloud.bs.psk.id          → BS PSK identity (RID 3)
cloud.bs.psk.key         → BS PSK secret (RID 5, opaque)
```

### Device Management (cloud.dm.*)
```
cloud.dm.uri             → DM server URI (pushed to devices, RID 0)
cloud.dm.lifetime        → Default registration lifetime (RID 1, default 86400)
cloud.dm.binding         → Default binding mode (RID 7, default "U")
cloud.dm.psk.id          → DM PSK identity (post-bootstrap)
cloud.dm.psk.key         → DM PSK key (post-bootstrap, opaque)
cloud.dm.lwm2m.version   → LwM2M version (default "1.1")
```

### Provision (per-device, stored in cloud.provision.configs JSON)
```
cloud.provision.request   → Endpoint name to provision (iot-cloudd watches)
cloud.provision.configs   → JSON blob keyed by endpoint name:
  {
    "urn:dev:gateway-42": {
      "sec.uri":      "coaps://...",    // OID 0 RID 0
      "sec.bs":       1,                // OID 0 RID 1
      "sec.mode":     0,                // OID 0 RID 2
      "sec.identity": "iot-client",     // OID 0 RID 3
      "sec.key":      "...",            // OID 0 RID 5
      "sec.ssid":     0,                // OID 0 RID 10
      "dm.psk.id":    "iot-dm-...",     // DM PSK per-device
      "dm.psk.key":   "...",            // DM PSK key per-device
      "srv.lifetime": 86400,            // OID 1 RID 1
      "srv.binding":  "U",              // OID 1 RID 7
      "lwm2m.version":"1.1"
    }
  }
cloud.deprovision.request → Endpoint name to deprovision (iot-cloudd watches)
```

### Endpoints (written by iot-cloudd, read by cloud UI)
```
cloud.endpoints          → JSON array of provisioned endpoints
  [{
    "endpoint":      "urn:dev:gateway-42",
    "state":         "online",
    "tun_ip":        "10.9.0.12",
    "proxy_port":    5001,
    "registered":    true,
    "last_seen_unix": 1718123456
  }]
```

### VPN Server (cloud.vpn.*)
```
cloud.vpn.subnet         → Tunnel subnet (default 10.9.0.0/24)
cloud.vpn.port.next      → Next proxy port (5001–6000)
cloud.vpn.ca.crt         → CA cert path
cloud.vpn.ca.key         → CA key path (secret volume)
cloud.vpn.server.crt     → Server cert path
cloud.vpn.server.key     → Server key path
```

### Services (services.cloud.*)
```
services.ds.state                           → ds-server state
services.cloud.iot.cloudd.enable|state      → iot-cloudd enable/state
services.cloud.iot.httpd.enable|state       → iot-httpd enable/state
services.cloud.openvpn.server.enable|state  → openvpn-server enable/state
```
All service keys use dots only. Daemons self-report state at startup/shutdown.

## Related modules

| Module | Role |
|--------|------|
| `modules/data-store` | ds-server + client lib (IPC backbone) |
| `modules/http-server` | iot-httpd — reused for both cloud and device |
| `modules/server/lwm2m` | LwM2M BS/DM server libs |
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
