# TDD Plan — Device Ownership Transfer (Advanced → Transfer)

Status: **PROPOSAL / DRAFT** — not yet implemented. Replaces the `panel='transfer'`
placeholder in `iot-ui/src/app/advanced/advanced.component.ts` ("Coming soon —
device configuration / credential transfer"). Build/test via **podman** (no
local C++/node toolchain on the dev Mac; image CI builds on `main` only).

This plan is deliberately phased: **Phase 1** ships a self-contained device-local
transfer (field swap), **Phase 2** adds the two-sided cloud release/claim
protocol + VPN cert revocation + data purge. Decide §7 before coding Phase 2.

## 1. Goal

Re-home a device from **customer A** to **customer B** — provably severing A's
credentials, trust, and data while re-pointing the device at B's bootstrap
server — **without a console visit** (network connectivity survives) and
**without touching the hardware serial** (it is the immutable endpoint).

"Customer" = a separate cloud instance (own runtime CA, own VPN subnet, own
`cloud.endpoints` registry). The design degrades to a multi-tenant single-cloud
variant (see §7).

Requirements:

1. **Sever A.** After transfer, A's cloud cannot reach, read, or command the
   device: BS/DM PSKs gone, VPN client cert untrusted, endpoint removed from A's
   registry, A's telemetry history purged.
2. **No data leakage A → B.** The device-side store-and-forward telemetry buffer
   and operator config are wiped; cloud A's 60-day history for this serial is
   purged on release.
3. **Keep connectivity.** WiFi / cellular / `net.*` survive so the device can
   reach B's bootstrap server on its own — Transfer is **not** a Factory Reset.
4. **Re-home to B** with the lowest practical touch (console → device-ui paste →
   signed transfer token → zero-touch; §6).
5. **Admin-gated + auditable** on every plane (device, cloud A, cloud B).
6. **Anti-theft** (Phase 2): a device cannot be silently re-homed without the
   releasing cloud's cooperation — unless the operator opts into local-only
   transfer (§7, the key decision).

## 2. Ownership model — what binds a device to a customer

| Binding | Where | Wiped on transfer? |
| --- | --- | --- |
| BS PSK (identity = raw serial, key) | device `iot.bs.psk.*` ↔ cloud `cloud.endpoint.credentials` | **yes** (both ends) |
| DM PSK (`rpi<serial>@cloud.local`) | device `iot.dm.psk.*` ↔ cloud creds | **yes** (both ends) |
| DM URI | device `iot.dm.uri` (bootstrap-delivered) | **yes** (re-derived from B's BS) |
| BS URI | device `iot.bs.uri` (commissioned) | **repointed** to B |
| VPN client cert (signed by A's runtime CA) | device `/etc/iot/vpn/*` ↔ A's CA | **yes** + **revoked** on A (Phase 2) |
| Endpoint registry row (tun_ip, proxy_port) | cloud A `cloud.endpoints` | **yes** (deprovision) |
| Telemetry | device Mongo buffer + cloud A 60-day Mongo | **yes** (both) |
| **Serial** (`iot.serial`) | device hardware | **no** — immutable, = endpoint |

The hardware serial is the one constant; everything else is customer-scoped
credential/trust/data and is destroyed or repointed.

## 3. Why a distinct action (vs Factory Reset / Deprovision)

| | Wipes creds | Keeps network | Severs old cloud trust | Re-homes new owner | Touch |
| --- | --- | --- | --- | --- | --- |
| **Deprovision** (cloud A) | cloud-side only | n/a | partial — **VPN cert NOT revoked today** | no | cloud-ui |
| **Factory Reset** | everything | ❌ drops off net | yes (device forgets) | no — needs console | console |
| **Transfer** (this TDD) | customer-scoped only | ✅ keeps WiFi/cell | yes (both ends) | yes, low/zero-touch | device-ui / token |

Transfer = Factory Reset's credential wipe **minus** the network wipe **plus** a
two-sided cloud handshake.

## 4. Key facts grounded in the current code

- **Trigger/`.path` pattern to reuse:** `POST /api/v1/system/reboot` /
  `/factory-reset` in `modules/http-server/src/handler.cpp:498,534` write
  `/run/iot/{reboot,factory-reset}.request`; root units
  `yocto/meta-iot/recipes-iot/lwm2m/files/iot-{reboot,factory-reset}.{path,service}`
  act on them. **Every such `.path` unit MUST also be listed in
  `90-iot.preset`** or first-boot `preset-all` disables it (the PR #394 bug —
  see the preset file header and `reference_systemd_preset_gotcha` memory).
- **Factory-reset wipe scope** (`files/iot-factory-reset`): stops `iot-ds`,
  `rm -f /var/lib/iot/data_store.lua`, `rm -rf /etc/iot/vpn`, then
  `systemctl reboot`. Transfer's script is a **selective** sibling of this.
- **Device credential keys** (`modules/data-store/schemas/iot.lua`):
  `iot.serial`, `iot.bs.uri`, `iot.bs.psk.identity/key`, `iot.dm.psk.identity/key`,
  `iot.dm.uri`, `iot.conn.state`. Network keys live in `wifi.lua` / `net.lua`
  (NOT wiped).
- **Re-bootstrap already exists.** `iot::should_rebootstrap`
  (`apps/src/provisioning_policy.cpp`, wired at `apps/src/main.cpp:1657`) re-runs
  bootstrap when DM creds fail; the client **parks** on `iot.bs.uri` + BS PSK
  when unprovisioned. Transfer wipes DM creds → the device naturally re-homes
  with **no new client FSM**.
- **Cloud deprovision** (`apps/cloud/server/src/main.cpp:839` `handle_deprovision`
  → `BootstrapProvisioner::deprovision` `modules/server/lwm2m/src/bootstrap.cpp:44`):
  `m_vpn_reg.release(ep)` + `m_ep_reg.remove(ep)`, then `remove_credential`
  (`cloud_credentials.cpp:143`) drops the per-endpoint creds. **It does NOT
  revoke the issued VPN cert** — confirmed: no CRL/`crl-verify` anywhere in
  `modules/server/openvpn`. This is the Phase-2 security gap.
- **VPN CA** (`modules/server/openvpn`, `CertAuthority::mint_client`) signs each
  device cert; server config built by `build_server_config`
  (`openvpn_server.cpp:45`) — would gain `crl-verify` (§5b D).
- **Telemetry sinks:** device Mongo store-and-forward buffer (per
  `tdd-vehicle-telemetry.md`); cloud `cloud.vehicle.telemetry` (volatile) +
  60-day Mongo history.
- **Auth note:** the reboot/factory-reset handlers default `access_level="Admin"`
  and only downgrade on a *valid non-Admin* session — i.e. an absent/invalid
  cookie still passes the Admin gate. The new transfer endpoint MUST **fail
  closed** (default deny) given it is destructive + ownership-changing. Fix the
  pattern for transfer (and back-port to reboot/factory-reset).

## 5. Design

### 5a. Device plane — selective wipe + park (Phase 1)

`POST /api/v1/system/transfer` (Admin, **fail-closed**) writes
`/run/iot/transfer.request`. A new root `iot-transfer.path` → `iot-transfer.service`
→ `iot-transfer` script performs:

**WIPE (customer-scoped identity + data):**
- ds keys: `iot.bs.psk.identity/key`, `iot.dm.psk.identity/key`, `iot.dm.uri`,
  `iot.conn.state`, and operator runtime config (`vpn.remote.*`, push/DNAT
  targets, any `cloud`-pushed keys). Implemented as a **keyspace allowlist wipe**
  via a ds-server op (preferred — see Task D) rather than deleting the whole
  `data_store.lua` (which would also drop network).
- files: `rm -rf /etc/iot/vpn/*` (old VPN trust).
- device telemetry buffer (Mongo collection) + rotate logs.

**KEEP:** `wifi.networks`, cellular APN, `net.*`, `iot.serial`, firmware/OTA
state.

**REPOINT:** set `iot.bs.uri` + `iot.bs.psk.*` to B's coordinates (from the
transfer request payload / token; §6), then let the existing park +
`should_rebootstrap` re-home. **No reboot required** — the client self-restarts
on BS-PSK change (existing `should_restart_on_psk_change` → `exit(0)` under
`Restart=always`), or `iot-transfer` restarts `iot-lwm2m-client`.

Device lands in B's fleet by reusing the *unchanged* PSK-provisioning bootstrap
path (`tdd-psk-provisioning.md`).

### 5b. Cloud plane (Phase 2)

**A releases** (cloud-ui Endpoints → "Transfer out"): existing deprovision **+**
- **Revoke the VPN cert** — add CRL support to `CertAuthority` (maintain
  `crl.pem`, `openssl ca -revoke` equivalent or a minimal CRL writer), persist it
  in ds (`cloud.vpn.crl.pem`, like the runtime PKI), and add `crl-verify` to
  `build_server_config`. Without this A's CA still trusts the old cert.
- **Purge** the serial's rows from `cloud.vehicle.telemetry` + 60-day Mongo
  history.
- **Audit** record (who/when/serial → B).

**B claims** (cloud-ui → "Claim device"): the **unchanged** provision flow
(`cloud.provision.request` + `cloud.provision.bs.psk` → mint DM PSK + VPN cert).
Optionally emit a **transfer token** (§6) instead of hand-entry.

## 6. Re-commission delivery (touch levels)

How B's `bs.uri` + BS PSK reach the device after the wipe:

1. **Console / device-ui Admin entry** — installer pastes B's BS URI + BS PSK
   (matches today's commissioning). Phase 1 default.
2. **Signed transfer token / QR** — B's cloud emits `{bs.uri, bs.psk, serial,
   expiry, sig}`; pasted/scanned into device-ui Advanced → Transfer. No
   credential typing, tamper-evident, single-use.
3. **Zero-touch** — A and B share a neutral bootstrap directory that redirects
   the device to B. Most infra; last.

## 7. Decisions to confirm before coding

| # | Decision | Options | Default proposal |
| --- | --- | --- | --- |
| D1 | **Anti-theft** | (a) Transfer requires cloud-A release; (b) local-Admin is sufficient | **(b) for Phase 1** (field swap), **(a) gate for Phase 2** via the token being issued only after A releases |
| D2 | **Customer model** | separate clouds / multi-tenant single cloud | **separate clouds** (matches one-VPN-subnet-per-cloud today) |
| D3 | **VPN revocation** | add CRL now / rely on device wiping its cert | **add CRL** (defense-in-depth; device wipe alone leaves A's CA trusting a leaked key) |
| D4 | **Wipe mechanism** | ds keyspace-allowlist op / delete `data_store.lua` selectively | **keyspace-allowlist ds op** (deleting the file drops network) |
| D5 | **Re-home trigger** | reuse `should_rebootstrap` park / explicit reboot | **reuse park** (no reboot) |

## 8. New data-store keys (`iot.lua` / `cloud.lua`)

| Key | Type | ACL | Notes |
| --- | --- | --- | --- |
| `iot.transfer.state` | string | `write_acl={"gid:engineer"}` | `idle`/`wiping`/`awaiting-commission` — device-ui surfaces it. |
| `iot.transfer.token` | opaque | `write_acl={"gid:engineer"}`, write-only | Optional B token (carrier; cleared after apply). |
| `cloud.vpn.crl.pem` | opaque | `gid:cloud-svc` | Runtime CRL, persisted like the runtime PKI PEMs. |
| `cloud.transfer.release.request` | string | `gid:cloud-svc` | Serial to release (cloud-ui → iot-cloudd), one-shot like provision/deprovision. |
| `cloud.transfer.audit` | string(JSON) | `gid:cloud-svc` | Append-only audit of releases/claims. |

## 9. TDD work breakdown

### Phase 1 — device-local transfer (field swap)

- **A. Transfer script + units** [PKG] — `files/iot-transfer`, `iot-transfer.path`
  (`PathExists=/run/iot/transfer.request`), `iot-transfer.service`
  (`/bin/sh -c` per the #384 PATH lesson). Add all to `SYSTEMD_SERVICE:${PN}-httpd`
  **and `90-iot.preset`** (PR #394 lesson). Install in `iot_git.bb` + `FILES`.
- **B. Selective-wipe ds op** [DS] — a ds-server "delete keys" / "reset to schema
  default" op over an **allowlist** (the customer-scoped keys), so the script
  doesn't nuke `data_store.lua`. Unit tests: allowlisted keys cleared, network
  keys + `iot.serial` retained.
- **C. `POST /api/v1/system/transfer`** [HTTP] — Admin **fail-closed**; writes
  `/run/iot/transfer.request` with the (optional) token. Mirror reboot handler;
  fix the default-Admin quirk (§4).
- **D. device-ui Advanced → Transfer** [UI] — replace the placeholder: warning +
  optional new-owner token field + confirm; Admin-gated; Clarity inputs (match
  the `clr-input-container` convention used elsewhere). Show `iot.transfer.state`.
- **E. Re-home verification** [APP] — confirm the wipe → park → `should_rebootstrap`
  path re-homes against a 2nd BS without a client code change (integration).

### Phase 2 — cloud release/claim + revocation

- **F. CRL in `CertAuthority`** [CLOUD] — generate/maintain `crl.pem`, revoke a
  serial, persist `cloud.vpn.crl.pem`; add `crl-verify` to `build_server_config`;
  `OpenVpnServer::reconfigure` already restarts on a rendered-config change so a
  CRL update propagates. Tests: revoked cert rejected.
- **G. Release backend** [CLOUD] — `cloud.transfer.release.request` watch →
  deprovision + revoke + purge telemetry (`cloud.vehicle.telemetry` + Mongo) +
  audit. One-shot trigger like provision/deprovision.
- **H. cloud-ui "Transfer out" / "Claim device"** [CLOUD/UI] — Endpoints actions;
  Claim reuses the provision form + optionally mints a transfer token.
- **I. Transfer token** [CLOUD/APP] — signed `{bs.uri,bs.psk,serial,expiry}`;
  device-ui paste path; single-use + expiry enforced device-side.

## 10. Suggested merge order (small PRs)

1. A+B+C (device wipe mechanism + endpoint) — behind the units, testable in podman.
2. D (device-ui Transfer panel) — unblocks manual field swaps.
3. E integration pass on HW (re-home to a 2nd BS).
4. F (CRL) — independent, security-hardening; ship even if Phase 2 UI lags.
5. G+H (cloud release/claim).
6. I (token / zero-touch).

## 11. Security analysis

- **Theft via local wipe (D1).** Phase-1 local transfer means physical-Admin can
  re-home a stolen device. Acceptable for owned-fleet field swaps; for
  anti-theft, gate the device on a B-token that B's cloud only issues after A's
  release (Phase 2) — tie `iot.transfer.token` validation to a signature chain.
- **Old VPN cert (D3).** Until CRL lands, a transferred device's leaked client
  key would still authenticate to A's VPN. Device-side `/etc/iot/vpn` wipe stops
  the *device* presenting it, but not a copied key — hence CRL.
- **Fail-closed endpoint (§4).** The transfer endpoint must default-deny; do not
  inherit the reboot handler's default-Admin behaviour.
- **Token replay.** Transfer token single-use + short expiry + bound to serial.
- **Audit.** Every release/claim recorded (`cloud.transfer.audit`) for dispute
  resolution on ownership.

## 12. Open items

- Exact customer-scoped **keyspace allowlist** for the wipe (enumerate against
  `iot.lua` + any `cloud`-pushed device keys) — must be exhaustive or A-config
  leaks to B.
- Whether device-side telemetry buffer purge needs `iot-vehicled`/Mongo
  coordination (stop writer first, like factory-reset stops `iot-ds`).
- Multi-tenant single-cloud variant (D2=b): "transfer" becomes a re-key +
  registry move within one cloud; no CRL needed (same CA) but tenant data
  isolation in `cloud.*` must be designed first.
