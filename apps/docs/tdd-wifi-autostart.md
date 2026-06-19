# TDD Plan — WiFi Client Auto-start on Boot + WPA-Enterprise Support

Status: **COMPLETE** (implementable scope) — device side only (Yocto image +
wifi-client daemon); no cloud/UI changes. Unit + build verified in podman:
wifi-client suite **103/103** green. The new WPA-Enterprise surface is tracked
as **REQ-WIFI-024** (014–023, 026 were already taken).

### Implementation progress

| Task | State | Notes |
| --- | --- | --- |
| A — WifiNetwork struct gains EAP fields | ✅ DONE | `process.hpp`: `eap`, `identity`, `password`, `phase2`, `ca_cert` added after `key_mgmt` (positional aggregate inits stay valid). |
| B — parse_networks parses/validates EAP | ✅ DONE | `process.cpp`: `key_mgmt=WPA-EAP` requires non-empty `identity`+`password`; `eap` defaults `PEAP`, `phase2` defaults `auth=MSCHAPV2`; `ca_cert` optional; `psk` not required. |
| C — build_wpa_supplicant_config emits EAP | ✅ DONE | `process.cpp`: shared quote-escape lambda; EAP `network={}` block emits `key_mgmt=WPA-EAP`/`eap`/`identity`/`password`/`phase2`/`ca_cert` and **no** `psk`. PSK/NONE output byte-identical. |
| D — unit tests | ✅ DONE | `test/process_test.cpp` REQ-WIFI-024: 8 EAP tests (parse OK, missing id/pw rejected, defaults, conf-gen no-psk, ca_cert-only-when-present, quote-escape) + seeded-default round-trip; PSK/NONE regressions green. |
| E — seed default in schema | ✅ DONE | `schemas/wifi.lua`: `wifi.networks` default `[]` → `[{"ssid":"changeme","key_mgmt":"WPA-PSK","psk":"changeme","priority":10}]`; doc comment extended with EAP shape. `schema_test.cpp` `documented_defaults_match` updated (JSON braces defeat the brace-naive `entry_body` helper → asserted against full schema text). |
| F — auto-enable service | ✅ DONE | `iot_git.bb`: `SYSTEMD_AUTO_ENABLE:${PN}-wifi-client` `disable` → `enable`. Ships `wifi-client.env`; unit already orders `After=iot-ds.service network-online.target`. **Update (PR #212):** `SYSTEMD_AUTO_ENABLE` alone is insufficient — the image runs `systemctl preset-all` on first boot, which reset the unit to `preset: disabled`. Now also requires the shipped `90-iot.preset` (`enable iot-wifi-client.service`) for the enablement to stick. Also added `SupplementaryGroups=iot` (open the `0660` ds socket) and removed `RuntimeDirectory=iot` (it wiped the shared `/run/iot` on stop). |
| G — docs | ✅ DONE | `DEPLOY.md` wifi section: auto-start-on-image note + all three JSON shapes (PSK / open / WPA-EAP). |

Test cmd (wifi-client), podman:
```sh
podman run --rm -v "$PWD":/src:Z -w /src/modules/wan/wifi/client \
  localhost/iot-cloud-builder:authfix bash -lc '
    apt-get update && apt-get install -y libgtest-dev
    mkdir -p build && cd build
    cmake .. -DACE_ROOT=/usr/local/ACE_TAO-7.0.0 -DBUILD_WIFI_CLIENT_TESTS=ON
    make -j4 wifi-client-tests && ./wifi-client-tests'
```
(`libgtest-dev` is not pre-installed in `iot-cloud-builder`; `apt-get update`
first or `find_package(GTest)` fails. The `:lp` httpd image has it baked in.)

## 6. Deferred / not done

- **On-device boot verification** on real RPi hardware (service comes up,
  `wifi.assoc.state` → `connected`, `wifi.dhcp.ip` populated). Needs hardware
  + a real AP; cannot be verified in podman.
- Cloud-UI / device-UI EAP form fields (keys already flow through ds).
- Per-device secure WiFi credential provisioning (placeholder default is
  world-readable in the image — fine for lab, not production).

## Companion fix — NTP sync on a WiFi-only, no-RTC board

Because `wlan0` is brought up **outside** systemd-networkd (by wpa_supplicant +
udhcpc here), networkd manages only `eth0`, which is `RequiredForOnline=yes` by
default. On a headless WiFi-only RPi the cable-less `eth0` pins networkd's global
`Online state: offline`, which **parks `systemd-timesyncd`** — it never sends an
NTP packet. The Pi has no RTC, so the clock stays at its stale save-file value
and every TLS handshake fails `certificate is not yet valid` (this is what broke
the OpenVPN tunnel until the clock was set by hand).

Fix: the image ships `meta-iot/.../files/10-iot-wired.network` (sorts before the
stock `80-wired.network`, wins the `eth*/en*` match) with
`RequiredForOnline=no`, so a cable-less `eth0` no longer drags the system
offline and timesyncd syncs over WiFi within seconds of boot. Durable across
reboots with no RTC: timesyncd's save-file restores a forward-only clock at
early boot, kept fresh now that NTP actually syncs. (PR #240.)

## Companion fix — WiFi power-save flapping (RPi brcmfmac)

The RPi3 brcmfmac (BCM43430) enables WiFi **power-save after each association**
(`dmesg`: `brcmf_cfg80211_set_power_mgmt: power save enabled`). On an idle link
the chip sleeps, misses beacons, gets disassociated, then reconnects — usually
to a **new DHCP lease**, so the LAN IP hops around and the OpenVPN tunnel +
LwM2M registration flap in lockstep (the cloud's device-UI DNAT toggles
`1↔0 rule(s)`). It is not reboots/crashes: the daemons show `NRestarts=0`.

Fix: the `udhcpc-ds.script` lease hook runs `iw dev <iface> set power_save off`
on every `bound`/`renew` (post-association, where the driver re-enables it), and
`iw` is pulled into the wifi-client package. Best-effort — it never breaks DHCP.

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
- `key_mgmt` — optional, default `"WPA-PSK"`. Only the exact value `WPA-EAP`
  triggers the WPA-Enterprise branch; `NONE` is an open network; any other
  value (incl. `WPA-PSK`) is PSK-style — `psk` is required and the `key_mgmt`
  string is propagated verbatim to wpa_supplicant.conf.
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
