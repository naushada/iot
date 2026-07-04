# Multi-tenant isolation — end-to-end validation recipe

Closes the gap between "merged" and "trustworthy in production" for the
multi-tenant cloud (TDD `tdd-multi-tenant-cloud.md`, PRs #483–#506). Two slices
shipped **compile/gtest-only, "needs tun validation"**: **P3b** (inter-tenant nft
drop, table `iot_tenant_isol`) and **P3c-ii** (per-client CCD static IPs). This
recipe exercises them against a live OpenVPN tun on the cloud VM and asserts the
hard boundary: **a device in tenant A cannot reach tenant B's tunnel IP.**

> The isolation guarantee is enforced at the **nft/routing layer**, not the app.
> So it can be proven with two OpenVPN *clients* — you do NOT need two physical
> RPis (Path B below).

## Concrete knobs (from the implementation)

| Thing | ds key / value | Notes |
|---|---|---|
| Enable multi-tenant VPN | `cloud.vpn.ccd.dir` = e.g. `/etc/openvpn/ccd` | **Empty = single-tenant NO-OP.** Non-empty gates P3c CCD + adds `client-config-dir` to the server config. `apps/cloud/server/src/main.cpp:742` |
| Tenant `/24` pool | `cloud.vpn.tenant.pool` (default `10.9.16.0/20`) | Each tenant auto-assigned a `/24` from it (P4a). `main.cpp:139` |
| Tenant registry | `cloud.tenants` = JSON array `[{"id","name",...}]` | Create/edit a tenant = `db/set cloud.tenants` (audited `tenant.update`). No dedicated REST route. |
| Provision-into-tenant carrier | `cloud.provision.tenant` = `<tenant_id>` then bump `cloud.provision.request` | Watcher mints creds tagged with that tenant. `main.cpp:1168` |
| nft isolation table | `iot_tenant_isol` (ip table) | Built by `build_tenant_isolation_rules(subnets)`, applied by `rebuild_tenant_isolation` (P3b). `main.cpp:159` |
| Per-tenant quota | tenant `max.devices` | Enforced at provision (P5a). |
| Cert tenant tag | client cert `/OU=<tenant>` | P5b; CN stays bare for CCD/device compat. |

All cloud state is in ds — use `ds-cli get <key>` / `ds-cli set <key> <val>` on the
cloud VM (ds socket per the cloud deploy; see `DEPLOY.md`). Console isolation is
tested through the REST API.

## Preconditions

- Cloud VM up (Vultr), `iot-cloudd` + OpenVPN server + `iot-httpd` running.
- SSH to the cloud VM; `ds-cli`, `nft`, `openvpn` present.
- **Path A:** two RPis you can provision. **Path B:** any Linux host that can run
  two `openvpn` client processes in separate netns (no 2nd device needed).
- Baseline: capture single-tenant working state first (regression anchor).

---

## Phase 0 — enable multi-tenant VPN

```bash
# on the cloud VM
ds-cli set cloud.vpn.tenant.pool 10.9.16.0/20        # (or accept the default)
ds-cli set cloud.vpn.ccd.dir     /etc/openvpn/ccd    # non-empty = ENABLE
# iot-cloudd rewrites the OpenVPN server config (adds client-config-dir) and
# creates the ccd dir. Restart openvpn-server so it re-reads the config:
systemctl restart openvpn-server@iot     # unit name per your deploy
```

**PASS:** `ds-cli get cloud.vpn.ccd.dir` is non-empty; the running OpenVPN
`server.conf` now contains `client-config-dir /etc/openvpn/ccd`; the dir exists
(mode 0750).

## Phase 1 — create two tenants

```bash
ds-cli set cloud.tenants '[{"id":"acme","name":"Acme"},{"id":"globex","name":"Globex"}]'
```

**PASS (subnet auto-assign, P4a):** within a few seconds each tenant has a
distinct `/24` from the pool — check the tenant registry / assignment:

```bash
ds-cli get cloud.tenants           # each entry now carries its assigned /24 (e.g. 10.9.16.0/24, 10.9.17.0/24)
```

Record `ACME_NET` and `GLOBEX_NET` (the two `/24`s) — used below.

## Phase 2 — provision one device into each tenant

For each device (serial `S`, into tenant `T`):

```bash
ds-cli set cloud.provision.tenant "$T"          # acme | globex
ds-cli set cloud.provision.request "$(printf '{"serial":"%s"}' "$S")"   # payload per your provision schema
# watcher mints tenant-tagged BS/DM PSK + VPN client cert (OU=<T>), assigns a
# static tunnel IP from T's /24, writes the CCD file keyed by cert CN.
```

**PASS:**
- `cloud.endpoints` shows each device row tagged with the right `tenant`.
- A CCD file exists per device: `ls /etc/openvpn/ccd/` → one file per cert CN,
  each containing `ifconfig-push <ip> <mask>` with `<ip>` **inside that tenant's
  `/24`** (`rebuild_tenant_ccd`, `main.cpp:241`).
- Cert OU carries the tenant:
  ```bash
  openssl x509 -in <device-client-cert>.pem -noout -subject   # → …/OU=acme/CN=<serial>
  ```

## Phase 3 — bring the tunnels up + confirm static IPs

- **Path A:** boot both RPis; each connects OpenVPN.
- **Path B (no 2nd device):** on a Linux host, run two clients in netns:
  ```bash
  # per fake device: its tenant-scoped cert/key + the cloud CA
  ip netns add t-acme;   ip netns exec t-acme   openvpn --config acme-client.ovpn   --dev tun-acme   &
  ip netns add t-globex; ip netns exec t-globex openvpn --config globex-client.ovpn --dev tun-globex &
  ```

**PASS:** each client is assigned its **CCD static IP** — the acme client's
tunnel IP ∈ `ACME_NET`, the globex client's ∈ `GLOBEX_NET`. Confirm on the client
(`ip addr show tun*`) and on the server (`cat /etc/openvpn/openvpn-status.log`).

## Phase 4 — apply + verify the nft isolation table (P3b)

```bash
# on the cloud VM
nft list table ip iot_tenant_isol
```

**PASS:** the table exists and contains DROP rules for **inter-tenant** forwarded
traffic — `ACME_NET → GLOBEX_NET` and vice-versa dropped, while same-tenant and
tenant↔cloud-services are accepted. (If the table is absent, `rebuild_tenant_isolation`
didn't fire — check `iot-cloudd` logs for "tenant isolation" and that `ccd.dir`
is set.)

## Phase 5 — THE isolation assertions (the whole point)

Let `ACME_IP` / `GLOBEX_IP` be the two tunnel IPs, `GW = 10.9.0.1` (VPN gw).

```bash
# 1. Cross-tenant MUST FAIL (dropped by iot_tenant_isol):
ip netns exec t-acme ping -c2 -W2 "$GLOBEX_IP"     # EXPECT: 100% loss
ip netns exec t-globex ping -c2 -W2 "$ACME_IP"     # EXPECT: 100% loss

# 2. Tenant → cloud services MUST WORK (same-tenant + tenant↔cloud allowed):
ip netns exec t-acme   ping -c2 -W2 "$GW"          # EXPECT: reachable
ip netns exec t-globex ping -c2 -W2 "$GW"          # EXPECT: reachable

# 3. (Path A/2-device-per-tenant only) same-tenant peer MUST WORK.
```

**PASS = cross-tenant is 100% loss AND cloud gateway is reachable.** This is the
production guarantee. A cross-tenant ping that *succeeds* is a hard FAIL — the
isolation table isn't doing its job.

> ICMP note: the VPN gateway reachability is the real liveness check; don't rely
> on pinging the cloud's public IP (ICMP is dropped there — a known red herring).

## Phase 6 — console read-isolation (D6/D7)

```bash
# tenant-scoped operator (auth.users.accounts[] with tenant_id="acme"):
curl -sk -u acme-op:… https://<cloud>/api/v1/cloud/endpoints | jq '[.[].tenant] | unique'
# EXPECT: ["acme"] only — never "globex"

# platform operator (tenant_id="*"):
curl -sk -u admin:… https://<cloud>/api/v1/cloud/endpoints | jq '[.[].tenant] | unique'
# EXPECT: ["acme","globex"] (sees all)
```

**PASS:** the acme operator sees only acme endpoints; the reverse-proxy
device-UI for a globex endpoint is unreachable to the acme session; the platform
operator sees everything. Enforced in `handler_cloud.cpp` / `handler.cpp:1645`,
not the UI.

## Phase 7 — quota (P5a)

```bash
# set acme max.devices to its current count, then try one more:
ds-cli set cloud.tenants '[{"id":"acme","name":"Acme","max.devices":1}, …]'
ds-cli set cloud.provision.tenant acme
ds-cli set cloud.provision.request '{"serial":"OVER_QUOTA"}'
```

**PASS:** the over-quota provision is rejected (error surfaced on
`cloud.provision.request` result / audit log); no new acme endpoint row.

## Phase 8 — migration + regression (must stay green)

```bash
ds-cli set cloud.vpn.ccd.dir ''      # disable multi-tenant VPN
systemctl restart openvpn-server@iot
```

**PASS:** with `ccd.dir` empty, `rebuild_tenant_ccd` / isolation are NO-OP, the
`iot_tenant_isol` table is not applied, and a **legacy single-tenant device**
(no tenant prefix) registers and gets a tunnel IP exactly as before. This proves
the default path is untouched.

---

## Result matrix (fill in)

| # | Check | Expect | Result |
|---|---|---|---|
| 1 | `ccd.dir` set → server config has `client-config-dir` | yes | |
| 2 | two tenants each get a distinct `/24` | yes | |
| 3 | device rows tenant-tagged; CCD file IP ∈ tenant `/24` | yes | |
| 4 | cert `/OU=<tenant>` | yes | |
| 5 | `iot_tenant_isol` table present with inter-tenant DROP | yes | |
| 6 | **cross-tenant ping** | **100% loss** | |
| 7 | tenant → VPN gw | reachable | |
| 8 | tenant-scoped API read | own tenant only | |
| 9 | platform operator API read | all tenants | |
| 10 | over-quota provision | rejected | |
| 11 | `ccd.dir` empty → single-tenant path | unchanged | |

**#6 is the keystone.** If any device in one tenant can reach another tenant's
tunnel IP, multi-tenant is NOT production-safe regardless of the rest.
