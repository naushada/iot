# TDD Plan — WiFi Client Auto-start on Boot + WPA-Enterprise Support

Status: **IN PROGRESS** — design + scope agreed. Device side only (Yocto
image + wifi-client daemon); no cloud/UI changes. Build/test via the existing
host-side gtest harness under `modules/wan/wifi/client/`.

### Implementation progress

| Task | State | Notes |
| --- | --- | --- |
| A — WifiNetwork struct gains EAP fields | ⬜ TODO | `process.hpp`: `identity`, `password`, `eap`, `phase2`, `ca_cert`. |
| B — parse_networks parses/validates EAP | ⬜ TODO | `process.cpp`: when `key_mgmt=WPA-EAP`, require `identity`+`password`, parse optional `eap`/`phase2`/`ca_cert`; do **not** require `psk`. |
| C — build_wpa_supplicant_config emits EAP | ⬜ TODO | `process.cpp`: EAP `network={}` block (no `psk`); PSK/NONE branches unchanged. |
| D — unit tests | ⬜ TODO | `test/process_test.cpp`: EAP parse OK, missing identity/password rejected, EAP conf-gen, PSK regression. |
| E — seed default in schema | ⬜ TODO | `schemas/wifi.lua`: `wifi.networks` default `[]` → default network; doc comment extended with EAP shape. |
| F — auto-enable service | ⬜ TODO | `yocto/meta-iot/recipes-iot/lwm2m/iot_git.bb`: `SYSTEMD_AUTO_ENABLE:${PN}-wifi-client` `disable` → `enable`. |
| G — docs | ⬜ TODO | `DEPLOY.md` wifi section: both JSON shapes + auto-start note. |

Test cmd (wifi-client): host-side gtest
`cmake .. -DACE_ROOT=/usr/local/ACE_TAO-7.0.0 && make -j wifi_client_test && ./wifi_client_test`
(exact target name confirmed at implementation time from the module
`CMakeLists.txt`).

## 1. Goal

On a fresh Raspberry Pi boot with the `iot` image, the `wifi-client` daemon
must come up **automatically** and connect using **default credentials seeded
into the data-store**, supporting **both** modes per-network:

- **WPA-PSK** (home/personal): `{ssid, psk}`
- **WPA-Enterprise / WPA-EAP** (corporate, identity + password):
  `{ssid, key_mgmt:"WPA-EAP", eap, identity, password, phase2}`

No operator step is required for the default network; the default is
overridable at runtime (cloud-UI / `ds-cli`).

## 2. Current state (verified)

- `iot-wifi-client.service` already exists and orders correctly
  (`After=`/`Wants=network-online.target iot-ds.service`, `Type=simple`,
  `Restart=on-failure`), but **ships disabled**:
  `SYSTEMD_AUTO_ENABLE:${PN}-wifi-client = "disable"` in `iot_git.bb`.
- The daemon reads **all** config from ds keys; credentials come solely from
  `wifi.networks` (default `"[]"` → daemon parks `disconnected`).
- `modules/wan/wifi/client/src/process.cpp` parses `{ssid, psk, priority,
  key_mgmt}` and handles `WPA-PSK` + `NONE` only. For any non-`NONE`
  `key_mgmt` it **requires `psk` and always emits `psk="..."`** — so
  `WPA-EAP` would produce a malformed `network={}` block today.
- `WifiNetwork` (`process.hpp:34`) has no EAP fields.
- wifi-client is in `packagegroup-iot-full` (always installed in the default
  image); only the systemd auto-enable gate keeps it from starting.

## 3. Design

### 3.1 JSON shapes (`wifi.networks`)

`wifi.networks` stays a JSON **string** (array of objects), schema-typed
`string`; shape validated in C++ (unchanged contract). Two entry shapes:

```jsonc
// PSK (default key_mgmt when omitted)
{ "ssid": "HomeAP", "key_mgmt": "WPA-PSK", "psk": "passphrase", "priority": 10 }

// Open network
{ "ssid": "GuestAP", "key_mgmt": "NONE" }

// WPA-Enterprise
{ "ssid": "CorpAP", "key_mgmt": "WPA-EAP", "eap": "PEAP",
  "identity": "user@corp", "password": "secret",
  "phase2": "auth=MSCHAPV2", "priority": 20 }
```

Field rules:
- `ssid` — required, non-empty (unchanged).
- `key_mgmt` — optional, default `"WPA-PSK"`. Recognised: `WPA-PSK`, `NONE`,
  `WPA-EAP` (alias `IEEE8021X` accepted, propagated verbatim).
- `psk` — required iff `key_mgmt` is PSK-like (not `NONE`, not `WPA-EAP`).
- `identity`, `password` — required iff `key_mgmt == WPA-EAP`.
- `eap` — optional, default `"PEAP"`. (`PEAP` / `TTLS` / `TLS` …)
- `phase2` — optional, default `"auth=MSCHAPV2"` for PEAP/TTLS.
- `ca_cert` — optional path; emitted only when present.
- `priority` — optional int, default 0 (unchanged).

### 3.2 wpa_supplicant.conf generation

`build_wpa_supplicant_config` gains an EAP branch. Existing PSK/NONE output is
byte-for-byte unchanged. EAP block:

```
network={
    ssid="CorpAP"
    key_mgmt=WPA-EAP
    eap=PEAP
    identity="user@corp"
    password="secret"
    phase2="auth=MSCHAPV2"
    ca_cert="/etc/iot/certs/corp-ca.pem"   # only if ca_cert present
    priority=20
}
```

All quoted string values reuse the existing `"`/`\` escape helper so a
mischievous identity/password can't break the conf parser.

### 3.3 Default seeding (schema default)

`wifi.networks` default changes from `"[]"` to a one-network JSON array in
`schemas/wifi.lua`. Semantics: the schema default applies **only while the key
is unset**. The moment anything `set`s `wifi.networks`, the persisted value
wins; the default reappears only on a fresh image / ds reset. This matches the
"default that an operator can override at runtime" requirement.

⚠️ **Security note:** this bakes a plaintext credential into
`/etc/iot/ds-schemas/wifi.lua` (world-readable in the image). Acceptable for a
lab / default AP; production deployments should provision per-device instead
(out of scope here). The committed default uses a placeholder
(`ssid="changeme"`, `psk="changeme"`) so no real secret lands in git.

### 3.4 Auto-start

Flip `SYSTEMD_AUTO_ENABLE:${PN}-wifi-client` from `disable` to `enable` in
`iot_git.bb`. No `.service` edit needed: existing
`After=/Wants=iot-ds.service network-online.target` guarantees ds-server is up
(so the schema default for `wifi.networks` resolves) before the daemon's first
read. The unit is long-running with `Restart=on-failure`, so it re-arms on
every boot — satisfying "start once after reboot".

## 4. Out of scope

- Cloud-UI / device-UI editing of EAP fields (the keys flow through ds, so the
  generic key editor already works; bespoke form is future work).
- Per-device secure provisioning of WiFi credentials (see security note).
- Certificate management / EAP-TLS client certs beyond an optional `ca_cert`
  path passthrough.

## 5. Test strategy (TDD)

Write/extend `test/process_test.cpp` **first**, then implement until green:

1. `ParseNetworks_Eap_Valid` — full EAP entry parses into the struct.
2. `ParseNetworks_Eap_MissingIdentity_Rejected` — sets `err_out`, returns {}.
3. `ParseNetworks_Eap_MissingPassword_Rejected`.
4. `ParseNetworks_Eap_DefaultsEapAndPhase2` — omitted `eap`/`phase2` default.
5. `BuildConfig_Eap_EmitsEapBlockNoPsk` — generated conf has `key_mgmt=WPA-EAP`,
   `identity=`, `password=`, `eap=`, and **no** `psk=`.
6. `BuildConfig_Eap_CaCertOnlyWhenPresent`.
7. Regression: existing PSK/NONE parse + conf-gen tests stay green.
8. `schema_test.cpp`: assert the new `wifi.networks` default is valid JSON and
   round-trips through `parse_networks` without error.
