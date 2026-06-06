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

## Directory layout

```
apps/cloud/
├── Dockerfile          # multi-stage build (Ubuntu builder + Alpine UI)
├── docker-compose.yml  # 3-service orchestration
├── run.sh              # podman/docker compose wrapper
├── server/             # iot-cloudd C++ source
│   └── src/main.cpp    # wires EndpointRegistry + VpnRegistry + BootstrapProvisioner
└── ui/                 # Angular 14 Cloud Operator Dashboard (Clarity)
    └── src/
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

## Key data store paths (cloud.*)

```
cloud.endpoints          → JSON array of provisioned endpoints
cloud.ep.<ep>.state      → "online"|"offline"|"bootstrapping"
cloud.ep.<ep>.tun.ip     → assigned tunnel IP (10.9.0.x)
cloud.ep.<ep>.tun.port   → proxy port (5001+)
cloud.provision.request  → ds-bump: write endpoint name to provision
cloud.deprovision.request → ds-bump: write endpoint name to deprovision
cloud.vpn.subnet         → tunnel subnet (default 10.9.0.0/24)
cloud.vpn.port.next      → next available proxy port
```

## Related modules

| Module | Role |
|--------|------|
| `modules/data-store` | ds-server + client lib (IPC backbone) |
| `modules/http-server` | iot-httpd — reused for both cloud and device |
| `modules/server/lwm2m` | LwM2M BS/DM server libs |
| `modules/server/openvpn` | VPN registry lib |
| `modules/server/web` | Device UI reverse proxy |
