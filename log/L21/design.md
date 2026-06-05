# L21 — Cloud Server: Multi-Tenant IoT Gateway Platform

> **Status:** Draft — design phase.  TDD to follow after approval.

## 1. Overview

A cloud server that manages N IoT gateways (devices), each behind NAT,
reachable through device-initiated OpenVPN tunnels.  The cloud is the
single LwM2M authority (Bootstrap + Device Management) and a reverse-proxy
for each device's local Device-UI.

```
┌──────────────────────────────────────────────────────────────────┐
│                        Cloud Server                              │
│                                                                  │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌────────────────┐   │
│  │ LwM2M BS │  │ LwM2M DM │  │ NftRules │  │   Web Proxy    │   │
│  │  Server  │  │  Server  │  │ (router) │  │  (server/web)  │   │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └───────┬────────┘   │
│       │              │             │                │            │
│       └──────────────┴──────┬──────┘                │            │
│                             │                       │            │
│                     ┌───────┴───────┐      ┌────────┴────────┐   │
│                     │  Data Store   │      │   Cloud UI      │   │
│                     │  (ds-server)  │      │  (apps/cloud/ui)│   │
│                     └───────────────┘      └─────────────────┘   │
│                                                                  │
│  OpenVPN Server ── tun0 ── 10.9.0.1/24                           │
│       │                                                          │
└───────┼──────────────────────────────────────────────────────────┘
        │
   ═════╪══════════════ Internet ═══════════════════════════════
        │
   ┌────┴──────┐    ┌──────────┐    ┌──────────┐
   │  Device 1 │    │ Device 2 │    │ Device N │
   │  (NAT)    │    │  (NAT)   │    │  (NAT)   │
   │           │    │          │    │          │
   │ LwM2M Cli │    │ LwM2M Cli│    │ LwM2M Cli│
   │ Device UI │    │Device UI │    │Device UI │
   └───────────┘    └──────────┘    └──────────┘
```

## 2. Directory Structure

```
iot/
├── apps/
│   ├── cloud/                    # NEW
│   │   ├── CMakeLists.txt        # top-level — adds subdirs
│   │   ├── server/               # cloud-side daemon
│   │   │   ├── CMakeLists.txt
│   │   │   ├── src/
│   │   │   │   └── main.cpp      # entry point — wires everything
│   │   │   └── schemas/
│   │   │       └── cloud.lua     # cloud.* data-store keys
│   │   └── ui/                    # Cloud operator dashboard
│   │       ├── package.json       # Angular 14 (same stack as iot-ui)
│   │       └── src/
│   │           └── ...
│   └── ...
├── modules/
│   └── server/                    # NEW — reusable server modules
│       ├── CMakeLists.txt         # builds all three libs
│       ├── lwm2m/                 # Multi-tenant LwM2M server
│       │   ├── CMakeLists.txt
│       │   ├── inc/server/lwm2m/
│       │   │   ├── bootstrap.hpp  # BS server — provisions devices
│       │   │   ├── management.hpp # DM server — register/observe/notify
│       │   │   └── endpoint_registry.hpp  # ep → tunnel IP map
│       │   └── src/
│       │       ├── bootstrap.cpp
│       │       ├── management.cpp
│       │       └── endpoint_registry.cpp
│       ├── openvpn/               # Multi-client VPN server manager
│       │   ├── CMakeLists.txt
│       │   ├── inc/server/openvpn/
│       │   │   └── vpn_registry.hpp  # ep → tun IP, port assignment
│       │   └── src/
│       │       └── vpn_registry.cpp
│       └── web/                   # Reverse proxy to device UIs
│           ├── CMakeLists.txt
│           ├── inc/server/web/
│           │   └── proxy.hpp      # HTTP proxy: /<ep>/ → tunnel IP:80
│           └── src/
│               └── proxy.cpp
├── modules/
│   ├── data-store/    # (existing) — reused as-is
│   ├── net/router/    # (existing) — extended for per-device DNAT rules
│   └── ...
└── modules/http-server/  # (existing) — reused as-is
```

## 3. Data Model (`schemas/cloud.lua`)

```
cloud.endpoints                    -- JSON array of registered endpoint IDs
cloud.ep.<ep>.state               -- "online" | "offline" | "bootstrapping"
cloud.ep.<ep>.tun.ip              -- assigned tunnel IP (e.g. 10.9.0.12)
cloud.ep.<ep>.tun.port            -- assigned cloud-side proxy port (5001+)
cloud.ep.<ep>.lwm2m.registered    -- boolean — LwM2M registration active
cloud.ep.<ep>.last.seen.unix      -- last Rx or keepalive timestamp
cloud.ep.<ep>.device_ui.url       -- https://cloud:5001/ (computed)
cloud.vpn.port.next               -- next available proxy port (counter)
cloud.vpn.subnet                   -- tunnel subnet (default 10.9.0.0/24)
```

## 4. Component Design

### 4.1 Endpoint Registry (`modules/server/lwm2m/endpoint_registry`)

In-memory map `std::unordered_map<std::string, EndpointInfo>` keyed by LwM2M
endpoint name.  Backed by the data store (cloud.ep.* keys).

```cpp
struct EndpointInfo {
    std::string ep;           // "urn:dev:gateway-42"
    std::string tun_ip;       // "10.9.0.12"
    uint16_t    proxy_port;   // 5001
    bool        registered;   // LwM2M registration active
    time_t      last_seen;
};
```

### 4.2 LwM2M Bootstrap Server (`modules/server/lwm2m/bootstrap`)

- Listens on `coaps://0.0.0.0:5684` (same port as existing lwm2m binary)
- When a device sends `POST /bs?ep=<ep>`:
  1. Assigns a tunnel IP from the VPN registry
  2. Assigns a proxy port (5000+)
  3. Writes Security Object (OID 0) + Server Object (OID 1) back to the device
  4. Updates `cloud.ep.<ep>.state = "bootstrapping"`

### 4.3 LwM2M DM Server (`modules/server/lwm2m/management`)

- Handles Register / Update / De-register
- Routes Read / Write / Execute / Observe / Notify per endpoint
- CoAP URI routing: `POST /3/0/6?ep=<ep>` → resolves `<ep>` in the endpoint
  registry, forwards/processes the LwM2M operation for that device
- Writes telemetry to data store: `cloud.ep.<ep>.last.seen.unix`
- All DM traffic flows over the established VPN tunnel

### 4.4 VPN Registry (`modules/server/openvpn/vpn_registry`)

- Manages OpenVPN server with per-client configurations
- Assigns tunnel IPs from `cloud.vpn.subnet` (10.9.0.0/24)
- Assigns proxy ports from 5000+ pool
- Monitors tunnel state (connected/disconnected) → updates `cloud.ep.<ep>.state`
- Each device runs `openvpn-client` (existing module) connecting to the cloud
  OpenVPN server

### 4.5 Web Proxy (`modules/server/web/proxy`)

- HTTP reverse proxy: `GET /<ep>/` → `http://{tunnel_ip}:80/`
- Uses the endpoint registry to resolve `<ep>` → `tun_ip`
- Accessible at `https://cloud:443/<ep>/` or `https://cloud:<proxy_port>/`
- Optionally enforces auth (session cookie from cloud UI)

### 4.6 nftables Rules (extends `modules/net/router`)

- Per-device DNAT rules managed by the VPN registry
- On device connect: `nft add rule ip nat PREROUTING tcp dport {proxy_port} dnat to {tunnel_ip}:443`
- On device disconnect: remove the rule
- Existing `net-router` daemon's nft apply pipeline is reused — a new rule
  type `cloud-dnat` is added to the schema

## 5. Request Flow

### 5.1 Device Bootstrap

```
Device                    Cloud Server
  │                           │
  │── POST /bs?ep=device-1 ──→│  (CoAPs, device-initiated over VPN)
  │                           │  1. Lookup/assign tun IP 10.9.0.12
  │                           │  2. Assign proxy port 5001
  │                           │  3. Push Security + Server objects
  │←── 2.04 Changed ──────────┤
  │                           │  cloud.ep.device-1.state = "online"
```

### 5.2 LwM2M Read (Operator → Cloud → Device)

```
Operator (Cloud UI)          Cloud Server                Device
  │                              │                          │
  │── POST /db/get?keys=... ────→│                          │
  │   {ep: "device-1"}           │                          │
  │                              │── GET /3/0/6?ep=device-1─→│ (CoAPs over tunnel)
  │                              │←── 2.05 Content ─────────┤
  │←── {value: 42} ─────────────┤                          │
```

### 5.3 Device-UI Access (Operator → Cloud Proxy → Device)

```
Browser                       Cloud Server                Device
  │                              │                          │
  │── https://cloud/device-1/ ──→│                          │
  │                              │  resolve ep → 10.9.0.12  │
  │                              │  (or nftables DNAT 5001)  │
  │                              │── http://10.9.0.12:80/ ──→│
  │                              │←── HTML/JS ──────────────┤
  │←── device-1 UI ─────────────┤                          │
```

### 5.4 REST-to-CoAP Proxy (`/api/v1/db/{get,set}?ep=<id>`)

The cloud's `iot-httpd` reuses the same `/api/v1/db/{get,set}` API surface
as the device.  When a request carries `?ep=<id>`, the handler routes it
through the LwM2M Management Router instead of the local data store.  When
`ep` is absent, the request falls through to the local `ds-server`.

```
GET /api/v1/db/get?ep=urn:dev:gateway-42&key=/3/0/6

   iot-httpd handler
        │
   ep present? ──No──→ ds-server (local cloud.* keys)
        │
       Yes
        │
        ▼
   EndpointRegistry::lookup_by_ep("urn:dev:gateway-42")
        │  → tun_ip = 10.9.0.12
        ▼
   ManagementRouter::route("GET /3/0/6 → 10.9.0.12")
        │
        ▼
   LwM2M DM Server
        │  coaps://10.9.0.12:5684/3/0/6
        ▼
   Device (behind NAT, via VPN tunnel)
```

The key design point: the REST API is identical whether you're querying a
device or the cloud store.  The `?ep=` query parameter is the switch:

| Request | Target |
|---------|--------|
| `GET /api/v1/db/get?key=iot.endpoint` | Local ds-server — cloud metadata |
| `GET /api/v1/db/get?ep=urn:dev:g42&key=/3/0/6` | Device via CoAP tunnel |
| `POST /api/v1/db/set?ep=urn:dev:g42` with pairs | Device via CoAP tunnel |
| `GET /api/v1/db/get?ep=urn:dev:g42&key=/3/0/6&timeout=30` | Device long-poll via CoAP Observe |

The cloud UI calls the same `/api/v1/db/get?ep=<id>` endpoints that the
device-level `iot-ui` calls locally — no new API surface needed.

## 6. Cloud UI (`apps/cloud/ui`)

Separate Angular 14 SPA using the same Clarity stack as `iot-ui/`.

### Pages

| Page       | Purpose |
|------------|---------|
| Dashboard  | N online/offline device count, tunnel status, aggregate telemetry |
| Device List| Table: endpoint, state, tun IP, proxy port, last seen, launch-UI button |
| Device Detail | Per-device: LwM2M objects, tunnel stats, recent events |
| Provision  | Pre-provision a new device (generate PSK, assign port range) |

### API

- `GET /api/v1/cloud/endpoints` — list all endpoints with status
- `GET /api/v1/cloud/endpoint/<ep>` — single endpoint detail
- `POST /api/v1/cloud/provision` — pre-provision a device
- `GET /api/v1/cloud/proxy/<ep>/` — proxy to device UI (or use nftables port)

## 7. Reuse of Existing Modules

| Existing Module | How It's Reused |
|----------------|-----------------|
| `modules/data-store` | Cloud state (endpoint registry, VPN map) stored as data-store keys |
| `modules/net/router` | nftables rule management extended with per-device DNAT rules |
| `modules/http-server` | Cloud REST API — the existing `iot-httpd` serves cloud endpoints |
| `modules/openvpn/client` | Runs on each device — unchanged |
| `iot-ui` (device) | Each device's local Device-UI — unchanged |

## 8. Build & Package

- `apps/cloud/CMakeLists.txt` adds `server/` as a subdirectory
- `modules/server/CMakeLists.txt` builds three static libs: `libcloud_lwm2m.a`,
  `libcloud_vpn.a`, `libcloud_web.a`
- A new Yocto recipe or package split: `iot-cloud` package containing the
  cloud server binary + cloud UI
- Cloud UI built via `./serve.sh build` and installed to `/usr/share/iot/cloud-www/`

## 9. Port Allocation

```
Purpose              Port      Notes
─────────────────────────────────────────
LwM2M Bootstrap      5684      coaps://cloud:5684
LwM2M DM             5683      coaps://cloud:5683
HTTP API             8080      iot-httpd (cloud REST)
Cloud UI             443       nginx or iot-httpd serving cloud UI
Device UI proxy      5001+     one per device, DNAT'd to tunnel IP:443
```

## 10. TDD Plan (next step)

1. **Endpoint Registry** — unit tests for ep → tun IP mapping, CRUD through data store
2. **VPN Registry** — unit tests for IP/port assignment, collision detection
3. **LwM2M Bootstrap** — integration test: simulated device bootstraps against cloud
4. **LwM2M DM** — integration test: register, read, observe cycle through the cloud
5. **Web Proxy** — unit test: HTTP request routing by endpoint
6. **nftables Rules** — integration test: device connect adds DNAT, disconnect removes
7. **Cloud UI** — component tests for dashboard and device list
