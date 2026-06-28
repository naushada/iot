# TDD — Shared multi-tenant cloud-iot

Status: **proposed** (design only; no code). Successor decision to the current
"separate cloud per customer" model (`apps/docs/tdd-device-transfer.md` D2).
Capacity headroom that makes this worth doing is measured in
`apps/docs/tdd-cloud-load-benchmark.md` (~100 reg/s, <2% CPU at 2000 devices on
one modest VM → ample room for multiple tenants per instance).

## 1. Problem

Today one cloud-iot deployment serves exactly one customer. Everything lives in
a single flat keyspace with no notion of tenant/organization:

- `cloud.endpoints`, `cloud.endpoint.credentials` — single global JSON arrays
- `cloud.bs.*`, `cloud.dm.*`, `cloud.bs.master.key` — global config/secrets
- `cloud.vpn.*` — one subnet (`10.9.0.0/24`), one proxy-port range, one CA
- `auth.users.accounts` — users with Admin/Viewer, **no** device ownership
- `GET /api/v1/cloud/endpoints` returns **all** endpoints to any logged-in user
  (`modules/http-server/src/handler_cloud.cpp`)
- device serial = global LwM2M endpoint = PSK identity basis → two customers
  with the same serial collide

Running many small fleets therefore means one VM per customer. We want one
cloud-iot to host **multiple organizations** with strong isolation.

## 2. Goals / non-goals

- **G1** A single deployment hosts N tenants; each tenant sees only its own
  devices, credentials, VPN, and console users.
- **G2** Strong isolation: no cross-tenant data reads, no cross-tenant network
  reachability over the VPN, no PSK/identity collisions.
- **G3** Backward compatible: an existing single-tenant deployment keeps working,
  migrated to an implicit `default` tenant with zero operator action.
- **G4** No regression to the device: existing fielded devices keep registering.
- **NG1** Not a billing/quota system (hooks only; metering is later).
- **NG2** Not horizontal scaling of the device plane (separate effort; one
  instance already has the headroom for multiple tenants).
- **NG3** Not per-tenant *custom* LwM2M object models (shared object catalog).

## 3. Decisions

| # | Decision | Options | Choice & why |
|---|---|---|---|
| D1 | Tenant identifier | UUID / operator slug | **short slug** (`acme`), 3–32 `[a-z0-9-]`; human-readable in keys, URLs, CNs |
| D2 | Credential store shape | per-tenant arrays / one array + `tenant` field | **one `cloud.endpoint.credentials` array, each row tagged `tenant`** — reuses the existing resolver path; tenant is *derived* from the matched row. Minimal blast radius. |
| D3 | Device→tenant resolution at DTLS handshake | encode tenant in identity / global identity→tenant index / lookup by matched cred row | **tenant-qualified PSK identity** (below) → unambiguous even with duplicate serials; the matched row confirms it |
| D4 | VPN isolation | N OpenVPN servers / one server + per-tenant subnet via CCD | **one OpenVPN server on a `/16`, per-tenant `/24` carved via client-config-dir static IPs + per-tenant nftables** — avoids N daemons; isolation enforced by nft |
| D5 | VPN PKI | per-tenant CA / one CA, tenant in cert CN | **one runtime CA, tenant in the client-cert CN/OU**; network isolation is the nft subnet boundary, not the CA. (Per-tenant CA is a later hardening option.) |
| D6 | Console authz | per-tenant user arrays / `tenant_id` on each account + role | **`tenant_id` + role on each `auth.users.accounts` entry**; a reserved `*` tenant = platform-operator (cross-tenant) |
| D7 | API surface | new `/api/v1/tenants/{t}/...` / keep paths, scope by session | **keep existing paths, scope every read/write by the session's tenant** server-side; add `/api/v1/tenants` admin CRUD only |
| D8 | Migration | flag day / implicit default tenant | **implicit `default`**: untagged rows/users belong to `default`; single-tenant deploys are unchanged |

## 4. Architecture

### 4.1 Tenant registry — `cloud.tenants`

New ds key, JSON array (mirrors `cloud.endpoints` shape):

```json
[{
  "id": "acme",
  "name": "Acme Corp",
  "vpn.subnet": "10.9.16.0/24",      // carved from the /16 pool
  "proxy.port.start": 11000,
  "proxy.port.end": 11099,
  "dm.uri": "coaps://cloud.example.com:5683",
  "bs.master.key": "<wrapped, optional per-tenant zero-touch master>",
  "status": "active"
}]
```

A `TenantRegistry` (in-memory + ds-mirror, same pattern as `EndpointRegistry`)
allocates non-overlapping subnets/port-ranges from a configured pool
(`cloud.vpn.pool = 10.9.0.0/16`, proxy `11000–65000`). `default` is seeded with
the legacy `10.9.0.0/24` + `10000–10050` so existing deployments are unchanged.

### 4.2 Data model — tenant tag, not new arrays (D2)

Each `cloud.endpoints` / `cloud.endpoint.credentials` row gains `"tenant":"acme"`
(absent ⇒ `default`). The existing in-memory `EndpointRegistry` /
`VpnRegistry` / `ClientRegistry` gain a `tenant` field on each record and a
secondary index `by_tenant`. No new top-level keys for device data — keeps the
sync/rehydrate paths (`sync_endpoints_to_ds`, the startup rehydrate) intact, just
tenant-aware.

### 4.3 Device → tenant resolution (D3) — the crux

A device must prove which tenant it is, and duplicate serials across tenants must
not collide. **Tenant-qualified identity:**

- Onboarding provisions the device's bootstrap `ep` as **`<tenant>:<serial>`**
  (e.g. `acme:000000fe26a4ff`) and its BS PSK identity as
  `sha256("<tenant>:<serial>")[:32]`.
- `POST /bs?ep=acme:<serial>` → the BS splits off the tenant, validates it
  against `cloud.tenants`, and provisions the DM account with a tenant-scoped DM
  identity `rpi<serial>@acme.cloud.local` (extends today's
  `rpi<serial>@cloud.local`; `format_dm_identity` /`serial_from_dm_identity` in
  `provisioning_policy.cpp` gain a tenant arg).
- `resolve_bs_psk` / `resolve_dm_psk` match the tenant-qualified identity against
  the (tenant-tagged) credential rows, so a row only authenticates within its
  tenant. Duplicate serials in different tenants → different identities → no
  collision.
- The matched row's `tenant` then drives everything downstream: which VPN subnet
  the device gets, which registry bucket it lands in, which `dm.uri` it’s told.

Backward compatibility: an `ep` with no `:` ⇒ `default` tenant + today's
identity scheme. Fielded devices keep working untouched.

### 4.4 VPN isolation (D4/D5)

- One OpenVPN server on the pool `/16`. Each device gets a **static tunnel IP
  from its tenant's `/24`** via client-config-dir (keyed by cert CN).
- Per-tenant **nftables** rules (extend `iot-cloudd`'s existing DNAT/forwarding
  in `apps/cloud/server/src/main.cpp:170`) drop inter-tenant traffic: a tenant's
  `/24` may reach only the cloud services + its own devices, never another
  tenant's `/24`.
- The device-UI reverse proxy (`handler_proxy.cpp`) already DNATs per endpoint;
  it becomes tenant-aware by resolving the endpoint within the session's tenant.
- Client certs carry the tenant in CN/OU (D5) for auditability; the hard
  boundary is the nft subnet rule.

### 4.5 Console & API (D6/D7)

- `auth.users.accounts[]` gains `tenant_id` + keeps `access` (Admin/Viewer).
  A reserved `tenant_id:"*"` = **platform operator** (manage tenants, see all).
- Session (`auth.cpp` `create_session`) carries `tenant_id`; every cloud handler
  (`handler_cloud.cpp`) filters reads and stamps writes with it. A non-`*` user
  can never reference another tenant's endpoint/credential/VPN row — enforced in
  the handler, not the UI.
- New admin-only `GET/POST/DELETE /api/v1/tenants` (platform operator) to CRUD
  tenants + allocate subnet/ports. Everything else keeps its path.

### 4.6 Secrets

- Per-tenant zero-touch master optional (`cloud.tenants[].bs.master.key`,
  same AES-256-GCM envelope + `IOT_BS_MASTER_KEK`); falls back to the global
  `cloud.bs.master.key` then the manual tier. (Note: the zero-touch *DTLS* path
  is currently broken — see tdd-cloud-load-benchmark.md §9 — fix that first if
  zero-touch onboarding is in scope.)

## 5. Migration & backward compatibility

1. Ship `cloud.tenants` seeded with `default` = legacy subnet/ports.
2. Untagged endpoint/credential/user rows ⇒ `default` (read-time default; no
   data rewrite required).
3. Single-tenant deployments behave exactly as today; `default`'s identity
   scheme is the legacy one (no `tenant:` prefix), so fielded devices are
   unaffected.
4. Opt in to multi-tenant by creating a second tenant + a scoped Admin user.

## 6. Phasing

- **P1 — model + read isolation:** `cloud.tenants` + `TenantRegistry`; `tenant`
  field on rows/registries; session `tenant_id`; API read filtering + write
  stamping. Single VPN subnet still (no device-plane change). *Ships isolation
  for the console without touching the device.*
- **P2 — device→tenant:** tenant-qualified `ep`/identity, tenant-scoped DM
  identity, resolver changes, onboarding/provisioning UI. Duplicate-serial-safe.
- **P3 — VPN isolation:** `/16` pool, per-tenant `/24` via CCD, per-tenant nft
  rules, tenant-aware proxy.
- **P4 — platform-operator console:** `/api/v1/tenants` CRUD, subnet/port
  allocator UI, per-tenant zero-touch master.
- **P5 — hardening:** per-tenant CA option, quotas/metering hooks, audit log.

## 7. Security considerations

- **Authz is server-side only** — the UI must never be the enforcement point
  (matches today's risk in `handler_cloud.cpp`).
- **No cross-tenant identity reuse** — the tenant-qualified identity + tenant-
  tagged row is the invariant; add a provisioning check that rejects a
  `(tenant, identity)` that already exists under a different tenant.
- **VPN blast radius** — inter-tenant deny is default; tested explicitly (a
  tenant-A device must fail to reach a tenant-B tunnel IP).
- **Secret isolation** — a tenant Admin can never read another tenant's PSKs
  (already write-only; the read filter must also bucket by tenant).
- **Platform operator (`*`)** is the only cross-tenant principal; its actions
  should be audit-logged (P5).

## 8. Open questions

- **OQ1 (needs input):** per-tenant CA from the start (stronger, more moving
  parts) vs shared CA + nft boundary (D5, simpler)? Recommend shared first.
- **OQ2:** tenant-qualified `ep` (`acme:serial`) vs a separate `?tenant=` query
  on `/bs` — the former survives intermediaries that strip queries; recommend
  the prefix.
- **OQ3:** do we need per-tenant `dm.uri`/hostnames (vanity domains) in v1, or
  is one shared host with tenant in the identity enough? Recommend shared host.
- **OQ4:** quota model (max devices/subnet size per tenant) — defer to P5?

## 9. Testing

- Unit: tenant split/validate of `ep`; `resolve_*_psk` tenant scoping incl.
  duplicate-serial-different-tenant; API read filter + write stamp.
- Integration (extend the load benchmark): two tenants, overlapping serials,
  assert each registers only within its tenant and lands in the right subnet;
  assert tenant-A cannot reach tenant-B's tunnel IP.
- Migration: a legacy single-tenant ds boots as `default`, fielded device
  (no `tenant:` prefix) still registers.
- Regression: the existing single-tenant e2e recipe unchanged.
