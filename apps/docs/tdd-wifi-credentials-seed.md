# TDD Plan — Build-time WiFi Credential Seed (`wifi_credentials.lua`)

Status: **COMPLETE** (implementable scope) — design + impl done. Builds on the
auto-start/EAP work (PR #204, merged). Device/Yocto side only. The new logic is
a small build-time generator, unit-tested in pure Python — **20/20 green**.
On-target verification (a real Yocto build with a dropped-in
`wifi_credentials.lua`) is deferred (needs the kas/RPi build environment).

### Implementation progress

| Task | State | Notes |
| --- | --- | --- |
| A — `gen_wifi_default.py` generator | ✅ DONE | Tokenizer + parser for the lua subset → `wifi.networks` JSON (`password`→`psk`; EAP passthrough) → in-place rewrite of the staged `wifi.lua` default. Safe single-quoted-lua escaping (round-trips `\`, `'`, `"`). |
| B — Python unit tests | ✅ DONE | `test_gen_wifi_default.py`, 20 tests: PSK/EAP/open, single/list, mapping, hard-error cases, comments, control-char rejection, schema rewrite isolates wifi.networks, escaping round-trip. |
| C — recipe wiring | ✅ DONE | `iot_git.bb`: `gen_wifi_default.py` in `SRC_URI`; conditional `file://wifi_credentials.lua` (present-only, so it's both optional and signature-tracked); `do_install:append` runs the generator on the staged `wifi.lua`; `do_install[depends] += python3-native`. |
| D — gitignore + sample | ✅ DONE | `.gitignore` the real file (+ `__pycache__/`); committed `wifi_credentials.lua.sample`. |

Test cmd: `python3 -m unittest discover -s yocto/meta-iot/recipes-iot/lwm2m/files -p 'test_gen_wifi_default.py'` (host-side, no toolchain).

## 1. Goal

Let a build integrator opt in with `IOT_WIFI_SEED = "1"` **and** drop a
**gitignored** `wifi_credentials.lua` into the recipe's designated dir; the Yocto
build reads it and bakes the credentials into the `wifi.networks` schema default
so a freshly-flashed image associates to the operator's AP with no runtime step.
When `IOT_WIFI_SEED` is unset/`"0"` (the default), the build is a no-op and the
committed `changeme` placeholder default stands ("don't care").

> **Why a variable and not "file present"?** An earlier version gated `SRC_URI`
> on `os.path.exists(... wifi_credentials.lua)`. That makes `do_fetch`'s basehash
> depend on parse-time filesystem state, which bitbake rejects as
> non-deterministic metadata ("the basehash value changed … on reparse"). The
> gate is now the `IOT_WIFI_SEED` variable (part of the signature), so the file's
> presence no longer perturbs the hash; set the var in `local.conf` / kas.

This keeps real WiFi credentials **out of source control** — the committed
`wifi.lua` always carries only the placeholder.

## 2. Designated dir + file

- `yocto/meta-iot/recipes-iot/lwm2m/files/wifi_credentials.lua` — the real
  file, **gitignored**.
- `…/files/wifi_credentials.lua.sample` — committed template.
- `…/files/gen_wifi_default.py` — committed generator (build tool).

## 3. Credential file format (lua)

Two accepted shapes (matching the operator's mental model):

```lua
-- (a) single network, simplest form. "password" maps to psk for WPA-PSK.
return { ssid = "cordoba_2G", password = "whatever" }

-- (b) explicit list (PSK / open / WPA-EAP), one table per network.
return {
  { ssid = "cordoba_2G", key_mgmt = "WPA-PSK", psk = "whatever", priority = 10 },
  { ssid = "Guest",      key_mgmt = "NONE" },
  { ssid = "Corp",       key_mgmt = "WPA-EAP",
    identity = "u@corp", password = "secret", eap = "PEAP" },
}
```

Field handling (mirrors `parse_wifi_networks` in the daemon):
- `ssid` — required, non-empty.
- `key_mgmt` — optional, default `"WPA-PSK"`.
- For PSK (default / non-EAP, non-NONE): credential comes from `psk`, or from
  `password` if `psk` is absent (the simple form). One of the two required.
- For `WPA-EAP`: `identity` + `password` required; `eap`/`phase2`/`ca_cert`
  optional (generator omits them so the daemon applies its own defaults —
  `eap=PEAP`, `phase2=auth=MSCHAPV2`).
- For `NONE`: no credential.
- `priority` — optional int, default `10` (matches the placeholder default).

The generator parses a **constrained lua subset** (table literals with
identifier keys, single/double-quoted string values, integer values, `--`
comments). It is NOT a full lua interpreter; anything outside the documented
shape is a hard error (fails the build loudly rather than shipping wrong creds).

## 4. Output + schema rewrite

The generator emits a compact JSON array identical in shape to what the daemon
consumes, e.g.:

```json
[{"ssid":"cordoba_2G","key_mgmt":"WPA-PSK","psk":"whatever","priority":10}]
```

then replaces the `wifi.networks` `default = '…'` line in the staged
`wifi.lua`. The JSON is embedded as a **single-quoted lua string** with `\` and
`'` escaped, so an SSID/PSK containing those characters round-trips byte-exact
through ds-server back to the daemon. Only the `${D}` (staged) copy is changed;
the committed source `wifi.lua` is untouched.

## 5. Recipe wiring (build-time, no-op when absent)

```python
# Variable-gated — keeps the file optional AND part of the recipe signature so a
# credential change triggers a rebuild, WITHOUT a parse-time os.path.exists()
# (which makes do_fetch's basehash non-deterministic; see §1 note).
IOT_WIFI_SEED ??= "0"
SRC_URI += "${@'file://wifi_credentials.lua' if d.getVar('IOT_WIFI_SEED') == '1' else ''}"
SRC_URI += " file://gen_wifi_default.py"

do_install:append() {
    if [ -f ${WORKDIR}/wifi_credentials.lua ]; then
        python3 ${WORKDIR}/gen_wifi_default.py \
            ${WORKDIR}/wifi_credentials.lua \
            ${D}${sysconfdir}/iot/ds-schemas/wifi.lua
        bbnote "wifi-client: seeded wifi.networks default from wifi_credentials.lua"
    fi
}
```

`cmake_do_install` has already placed `wifi.lua` at
`${D}${sysconfdir}/iot/ds-schemas/wifi.lua`; the append rewrites it in place.

## 6. Test strategy (TDD)

`test_gen_wifi_default.py` (pure Python, no toolchain), write first:

1. `parse_simple_psk` — `{ssid, password}` → one WPA-PSK network, `password`
   mapped to `psk`, `priority` defaulted to 10.
2. `parse_explicit_psk_psk_field` — `psk` field used directly.
3. `parse_list_multi` — array of tables preserves order + fields.
4. `parse_open_none` — `key_mgmt="NONE"` → no psk.
5. `parse_eap` — identity+password retained; eap omitted unless given.
6. `reject_missing_ssid`, `reject_psk_without_credential`,
   `reject_eap_without_identity` — hard errors.
7. `ignores_lua_comments`.
8. `json_output_is_valid_and_compact`.
9. `escaping` — ssid/psk with `'`, `\`, `"` round-trips through the
   single-quoted lua literal (parse the rewritten schema's default back out).
10. `rewrite_targets_only_wifi_networks` — other keys' `default =` lines
    untouched; rewritten file still contains exactly one wifi.networks default.

## 7. Out of scope / caveats

- Credential remains **plaintext on the device image** — this moves it out of
  git, not off the device.
- One build = one credential set for every device flashed from that image
  (fine for a shared/default AP; per-device provisioning is separate).
- Not a general lua interpreter — only the documented credential shapes.
