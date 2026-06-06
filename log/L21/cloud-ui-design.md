# L21 — Cloud UI Design

## Reuse Strategy

Maximum reuse from `iot-ui/`.  The cloud UI is a fork of the device UI
scaffold — identical common layer, different app pages.

### Files Copied As-Is (symlink or duplicate)

```
cloud-ui/
├── package.json              ← iot-ui/package.json (identical)
├── angular.json              ← iot-ui/angular.json (identical)
├── tsconfig.json             ← iot-ui/tsconfig.json (identical)
├── tsconfig.app.json         ← iot-ui/tsconfig.app.json (identical)
├── proxy.conf.json           ← iot-ui/proxy.conf.json (identical)
├── .gitignore                ← iot-ui/.gitignore (identical)
├── Containerfile             ← iot-ui/Containerfile (identical)
├── serve.sh                  ← iot-ui/serve.sh (identical)
├── src/
│   ├── environments/         ← iot-ui/src/environments/ (identical)
│   ├── favicon.svg           ← iot-ui/src/favicon.svg (identical)
│   ├── assets/iot-logo.svg   ← iot-ui/src/assets/iot-logo.svg (identical)
│   ├── polyfills.ts          ← iot-ui/src/polyfills.ts (identical)
│   ├── main.ts               ← iot-ui/src/main.ts (identical)
│   ├── index.html            ← iot-ui/src/index.html (title: "IoT Cloud")
│   └── common/               ← ALL files copied from iot-ui/src/common/
│       ├── app-globals.ts    (extended with cloud API types)
│       ├── httpsvc.service.ts(extended with cloud endpoints)
│       ├── session.service.ts
│       ├── sso-auth.guard.ts
│       ├── sso-auth.interceptor.ts
│       ├── pubsubsvc.service.ts
│       ├── longpoll.service.ts
│       ├── toast.service.ts
│       ├── theme.service.ts
│       └── validators.ts
```

### Reuse Summary

| Layer | Reuse % | What changes |
|-------|---------|--------------|
| Build tooling | 100% | Nothing |
| Common services | 90% | app-globals + httpsvc extended |
| App shell | 60% | Sidebar menu items differ, live strip simpler |
| Feature pages | 0% | All new: dashboard, endpoints, provision |

## App-Specific Files

```
cloud-ui/src/app/
├── app.module.ts           # Same structure, different component list
├── app-routing.module.ts   # Routes: dashboard, endpoints, provision, logs
├── app.component.ts        # <router-outlet><app-toast> (identical)
├── login/                  # Copied from iot-ui/login (identical auth flow)
│   ├── login.component.ts/html/scss
├── main/                   # Layout shell — adapted sidebar menu
│   ├── main.component.ts/html/scss
├── dashboard/              # Cloud dashboard — device count, status
│   ├── dashboard.component.ts/html/scss
├── endpoints/              # Endpoint list table
│   ├── endpoint-list.component.ts
├── provision/              # Provision new device form
│   ├── provision.component.ts/html/scss
└── common/
    ├── toast/              # Copied from iot-ui
    │   └── toast.component.ts
    └── status-badge/       # Copied from iot-ui
        └── status-badge.component.ts
```

## Page Designs

### 1. Dashboard

```
┌──────────────────────────────────────────────────────────────┐
│ Cloud Dashboard                                              │
│                                                              │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌───────────────┐   │
│  │ Online   │ │ Offline  │ │ Tunnel   │ │ Provisioned   │   │
│  │    5     │ │    2     │ │ Active   │ │     7         │   │
│  │ devices  │ │ devices  │ │   5/7    │ │   total       │   │
│  └──────────┘ └──────────┘ └──────────┘ └───────────────┘   │
│                                                              │
│  Recent Activity                                             │
│  ┌──────────────────────────────────────────────────────┐    │
│  │ urn:dev:gateway-42  online  10.9.0.12:5001  2m ago  │    │
│  │ urn:dev:gateway-37  offline 10.9.0.8:5005   1h ago  │    │
│  └──────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────┘
```

Status cards: same `status-card` CSS from iot-ui.  Data from
`GET /api/v1/cloud/endpoints`.

### 2. Endpoint List

```
┌──────────────────────────────────────────────────────────────┐
│ Endpoints                                         [+ Provision]│
│                                                              │
│  clr-datagrid:                                               │
│  ┌──────────────────────────────────────────────────────┐    │
│  │ Endpoint          │ State  │ Tun IP   │ Port │ Action │    │
│  │ urn:dev:gateway-42│ online │ 10.9.0.12│ 5001 │ [UI→] │    │
│  │ urn:dev:gateway-37│ offline│ 10.9.0.8 │ 5005 │ [UI→] │    │
│  └──────────────────────────────────────────────────────┘    │
│                        7 endpoints                            │
└──────────────────────────────────────────────────────────────┘
```

`clr-datagrid` pattern — same as iot-ui scan results / services list.
"Launch UI" button opens `https://cloud:<proxy_port>/` in new tab.

### 3. Provision

```
┌──────────────────────────────────────────────────────────────┐
│ Provision Device                                             │
│                                                              │
│  form-grid (4-col):                                          │
│  ┌──────────────────────────────────────────────────────┐    │
│  │ Endpoint Name   [urn:dev:gateway-42           ]      │    │
│  │                                         [Provision]  │    │
│  └──────────────────────────────────────────────────────┘    │
│                                                              │
│  Result (after provision):                                   │
│  ┌──────────────────────────────────────────────────────┐    │
│  │ Endpoint: urn:dev:gateway-42                         │    │
│  │ Tunnel IP: 10.9.0.12                                 │    │
│  │ Proxy Port: 5001                                     │    │
│  │ Security TLV: (hex dump)                             │    │
│  │ Server TLV:   (hex dump)                             │    │
│  └──────────────────────────────────────────────────────┘    │
└──────────────────────────────────────────────────────────────┘
```

Calls `POST /api/v1/cloud/provision` with `{"endpoint":"..."}`.

### 4. Sidebar

```
┌──────────────┐
│ IoT Cloud    │
│              │
│ ◆ Dashboard  │
│ ▶ Endpoints  │
│ ▶ Provision  │
│ ▶ Logs       │
│              │
│              │
│ ☀ Dark       │
│ ← Sign out   │
└──────────────┘
```

Same collapsible tree structure as iot-ui — but only 4 menu items,
no children needed (simpler than device UI).

## API Endpoints (already built in TDD #7)

| Method | Path | Purpose |
|--------|------|---------|
| GET | /api/v1/cloud/endpoints | List all devices |
| GET | /api/v1/cloud/endpoint?ep= | Single device detail |
| POST | /api/v1/cloud/provision | Provision new device |
| POST | /api/v1/cloud/deprovision | Remove device |

Plus all existing `/api/v1/auth/*`, `/api/v1/status`, `/api/v1/log`,
`/api/v1/db/*` endpoints inherited from iot-httpd.

## Data Flow

```
Cloud UI (Angular SPA)
    │
    ├── GET /api/v1/cloud/endpoints → cloud.* keys in ds-server
    ├── POST /api/v1/cloud/provision → BootstrapProvisioner → VPN alloc
    ├── GET /api/v1/db/get?ep=device-1&key=/3/0/6 → CoAP to device
    └── https://cloud:5001/ → nftables DNAT → tunnel IP:443 → Device UI

Device push → Cloud LwM2M server writes directly to ds-server:
  
  Device                    lwm2m (cloud)              ds-server
    │                           │                         │
    │── CoAP /push?ep=d1 ──────→│                         │
    │   {"/3/0/6": 42}          │── ds->set(ep.d1./3/0/6, 42) ──→│
    │←── 2.04 ──────────────────│                         │
```

No HTTP hops between cloud daemons — all IPC via data_store::Client → ds-server.
Same pattern as device daemons (openvpn-client writes vpn.state, net-router writes
net.iface.active).

All feature pages (VPN, WAN, Routing, LwM2M, Services) use the same HttpsvcService
pattern from iot-ui — same API calls, same data flow, same long-poll and toast.

## Implementation Plan

1. Copy `iot-ui/` → `apps/cloud/ui/`
2. Remove device-specific app pages (vpn/, wan/, routing/, lwm2m/, services/)
3. Replace app/main/ sidebar menu
4. Build cloud pages: dashboard, endpoint-list, provision
5. Extend common/app-globals.ts with cloud types
6. Extend common/httpsvc.service.ts with cloud API calls
7. Test with `./serve.sh`
