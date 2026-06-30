# TDD — Shared multi-tenant cloud-iot

Status: **in progress** (implementation landing in slices). Successor decision
to the current "separate cloud per customer" model
(`apps/docs/tdd-device-transfer.md` D2). Capacity headroom that makes this worth
doing is measured in `apps/docs/tdd-cloud-load-benchmark.md` (~100 reg/s, <2%
CPU at 2000 devices on one modest VM → ample room for multiple tenants per
instance).

## Implementation progress

| Slice | What | PR | Status |
|---|---|---|---|
| P1a | Core primitives (`tenant_policy`, `cloud.tenants` schema) | #484 | ✅ merged |
| P1b | Tenant-aware BS/DM PSK resolvers (default == legacy) | #485 | ✅ merged |
| P2a | BS `provisioning_resolver` (device→tenant onboarding); e2e 200/200 | #486 | ✅ merged |
| P1c | Console read-isolation: session `tenant` + scoped `/api/v1/cloud/*` | #487 | ✅ merged |
| P1d | `iot-cloudd` row-tagging: `cloud.endpoints` rows carry `tenant` | #488 | ✅ merged |
| P3a | Per-tenant VPN **subnet math + nft isolation rules** (pure, gtest) | _this_ | 🔵 |
| P1e | `db/get cloud.endpoints` tenant scoping (**live UI isolation**) | _this_ | 🔵 |
| P3b | Apply inter-tenant nft isolation table (compile-verified) | _this_ | 🔵 needs tun validation |
| P3c | OpenVPN `/16` + per-client CCD static IP from tenant `/24` | — | ⏭️ needs tun validation |
| P4a | Tenant registry: auto VPN-subnet assignment (`cloud.tenants` watch) | _this_ | 🔵 |
| P4b | Tenant-aware provision *watcher* + cred minting | #494 | ✅ merged |
| P5a | Per-tenant device **quota** (`max.devices`), enforced at provision | #495 | ✅ merged |
| P3c-bb | P3c building blocks: per-tenant IP alloc + OpenVPN CCD (inert) | #496 | ✅ merged |
| **PB** | **Adopt Option B (device-agnostic tenancy): bare identities, tenant from cred-row tag, `cloud.provision.tenant` carrier; revert qualified `ep`/identity** | _this_ | 🔵 gtest |
| P4c | Operator console UI: **Tenants CRUD page** + Endpoints **tenant selector** (`provision.tenant`) | _this_ | 🟡 needs node build/validation |
| P5b | Per-tenant CA / cert namespace | — | ⏭️ needs tun validation |
| P5c | **Audit log** of operator + provisioning actions (`cloud.audit.log`, iot-httpd db/set hook + read-only UI) | _this_ | 🟢 gtest + ng-build |

**P3b/P3c split:** P3b applies the inter-tenant **nft drop** table
(`build_tenant_isolation_rules` over the tenant subnets) via the same
`apply_ruleset` path as the device DNAT — compile-verified, but the live nft
effect needs a NET_ADMIN/netfilter host. The rules only *bite* once each
device's tunnel IP comes from its tenant's `/24`, which is **P3c**: per-client
OpenVPN client-config-dir static IPs + `VpnRegistry` allocating from the
tenant subnet. P3c is the larger, tun-validated piece.

**P4a:** an operator creates a tenant by `db/set`-ing `{id, name}` into
`cloud.tenants`; `iot-cloudd` watches the key and carves a non-overlapping `/24`
from `cloud.vpn.tenant.pool` (default `10.9.16.0/20`, clear of the legacy
default `10.9.0.0/24`) for any tenant lacking one — the same ds-driven reconcile
pattern as the rest of the cloud. Pure carving/assignment is unit-tested
(`assign_missing_subnets`); applying the per-tenant nft isolation (P3a's
`build_tenant_isolation_rules`) is part of P3b (needs tun).

With P1e the **console is tenant-isolated end to end**: `iot-cloudd` tags
`cloud.endpoints` rows (P1d), and both the cloud-API handler (P1c) and the
generic `db/get` the cloud-ui actually uses (P1e) filter to the session's
tenant. A platform operator (`tenant = "*"`) still sees all.

P3 splits into **P3a** (the pure allocation/rule-generation core — landed here,
fully unit-tested) and **P3b** (the live OpenVPN client-config-dir + nftables
*application*, which can only be validated against a real `tun`/OpenVPN
environment, so it is deferred to HW/integration validation rather than merged
blind — same discipline as the device-side fixes marked "needs HW validation").

Every slice keeps the **default tenant byte-identical to today**, so existing
single-tenant deployments and fielded devices are unaffected (verified by gtest
+ a 200/200 default-tenant load run per slice).

> **Design change — Option B (device-agnostic), adopted 2026-06-29.** The
> original design encoded the tenant into the device's identity
> (`ep=<tenant>:<serial>`, `sha256("tenant:serial")[:32]`). On review that
> over-coupled the device to cloud org structure: it only existed to keep the
> DTLS-PSK identity unique when two tenants share a serial. We instead **require
> globally-unique device serials** and keep the device **fully tenant-agnostic**
> — it bootstraps with its bare serial exactly as single-tenant. The tenant
> lives **only in the matched `cloud.endpoint.credentials` row's `tenant` tag**;
> the cloud derives it from there. This decouples the device, makes tenant
> transfer a pure cloud-side row edit, and keeps the default tenant automatically
> byte-identical. The sections below reflect Option B; D3 records the switch.

**Endpoint keying (Option B).** Identities are **bare** everywhere
(`sha256(serial)[:32]`, `rpi<serial>@cloud.local`) and the registry/DNAT key by
the **bare serial** — globally unique, so it matches what every device registers
`/rd` as, regardless of tenant. The provision *watcher* reads the target tenant
from a `cloud.provision.tenant` carrier (set by the operator console alongside
`cloud.provision.request`) and tags the cred + registry rows; the startup heal
tags from the existing row's `tenant`.

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
| D3 | Device→tenant resolution at DTLS handshake | encode tenant in identity / global identity→tenant index / lookup by matched cred row | **lookup by matched cred row (Option B, device-agnostic)** — device presents its bare serial; the tenant is the matched row's `tenant` tag. Requires globally-unique serials (documented constraint). Decouples the device from org structure; transfer = a cloud-side row edit. *(Superseded the original "tenant-qualified PSK identity" choice on 2026-06-29 — that only bought duplicate-serial support, at the cost of coupling the device.)* |
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

### 4.3 Device → tenant resolution (D3) — the crux (Option B, device-agnostic)

The device is **tenant-agnostic** — it bootstraps with its **bare serial**,
exactly as single-tenant. Globally-unique serials make this unambiguous; the
tenant is the matched credential row's tag.

- Onboarding provisions a `cloud.endpoint.credentials` row keyed by the bare
  serial, **tagged `"tenant":"acme"`**, with **bare** identities (BS
  `sha256(serial)[:32]`, DM `rpi<serial>@cloud.local`). The operator console
  selects the tenant via the `cloud.provision.tenant` carrier — the device never
  learns it.
- `POST /bs?ep=<serial>` → `plan_bs_account` matches the row by serial, reads its
  `tenant` tag, validates it against `cloud.tenants`, and provisions the DM
  account with the **bare** DM identity + the **per-tenant `dm.uri`**.
- `resolve_bs_psk` / `resolve_dm_psk` match the **bare** identity against the
  credential rows and return the key; the tenant isn't needed for key selection
  (it's read from the matched row downstream). Globally-unique serials guarantee
  a single match.
- The matched row's `tenant` then drives everything downstream: which VPN subnet
  the device gets, which registry bucket it lands in, which `dm.uri` it's told.

**Duplicate serials across tenants are disallowed** (the documented constraint
that buys this simplicity). The quota/provision path can reject a serial already
present under another tenant. Backward compatibility is automatic: an untagged
row ⇒ `default` tenant + today's exact bytes; fielded devices are untouched.

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
