# RAUC A/B image OTA — device bring-up checklist

Status: **wired; build-side validation in progress — pending hardware
validation.** The build/bootloader pieces below are in the tree (behind the
`IOT_AB=1` switch). The A/B layer config now **parses cleanly** for
`raspberrypi3-64` (all of meta-rauc / meta-lts-mixins / meta-rauc-raspberrypi
load, no MACHINE-compat skip); a full `IOT_AB=1 ./build.sh` is being run to
confirm the image + signed `.raucb` assemble. What then remains is booting an
RPi to verify the boot-select/rollback round-trip (the **Acceptance** section).
The runtime hooks (chunked `.raucb` upload, `iot-swupdate` → `rauc install`
routing, `iot-ota-confirm` mark-good/rollback, the `iot.boot.*` ds keys,
device-ui bank status) were already present.

**Build it:** `IOT_AB=1 ./build.sh` (or `kas build yocto/kas-ab.yml`). The plain
`./build.sh` still makes the proven single-rootfs image — the A/B path is opt-in
so an unvalidated bootloader never becomes the default.

## 1. Layers ✅
- Three layers are cloned in `yocto/Containerfile` and added to `bblayers` only
  for `IOT_AB=1` builds (`entrypoint.sh`); the kas path adds them via
  `yocto/kas-ab.yml`:
  - **`meta-rauc`** — the updater + the `bundle` class.
  - **`meta-lts-mixins`** (branch `scarthgap/u-boot`, collection
    `lts-u-boot-mixin`) — the newer u-boot 2024.04 that the next layer
    hard-depends on. **Must be added before `meta-rauc-raspberrypi`** or layer
    parsing fails (`depends on layer 'lts-u-boot-mixin' … not enabled`).
  - **`meta-rauc-community/meta-rauc-raspberrypi`** — the RPi u-boot
    boot-select integration (`rpi-u-boot-scr` `boot.cmd`).
- Add order in `entrypoint.sh`: `meta-rauc` → `meta-lts-mixins` →
  `meta-rauc-raspberrypi`.
- Note: the published sstate cache image (`IOT_USE_CACHE=1`) predates these
  layers, so a cold A/B build must rebuild the builder image (plain
  `IOT_AB=1 ./build.sh`, no cache) to clone them.

## 2. Bootloader (u-boot) ✅
- `RPI_USE_U_BOOT = "1"` is set in the A/B local.conf block. The community
  layer's `rpi-u-boot-scr` `boot.cmd` provides `BOOT_ORDER` + `BOOT_A_LEFT`/
  `BOOT_B_LEFT` bootcount; `boot-attempts=3` in `system.conf` matches its
  default. boot.cmd selects the bank by partition number (p2=A, p3=B).

## 3. Image ✅
- `WKS_FILE = "iot-ab.wks.in"` (4-partition layout, `/boot` bumped to 100M for
  u-boot). `IMAGE_FSTYPES` adds `ext4` (the bundle's rootfs slot) alongside
  `wic.bz2`. The `data` partition gets an automatic wic fstab entry mounting
  `/var/lib/iot` (ds state + certs survive a bank swap).

## 4. RAUC config + keys ✅
- `dynamic-layers/rauc/recipes-core/rauc/rauc-conf.bbappend` supplies `system.conf`
  → `/etc/rauc/system.conf` and the keyring → `/etc/rauc/keyring.pem` (via the
  meta-rauc `rauc-conf` recipe, which owns those paths — shipping them from the
  rauc daemon recipe too caused a do_rootfs file clash)
  (`compatible=iot-rpi`). (Under `dynamic-layers/rauc/` + `BBFILES_DYNAMIC` so it
  only parses when meta-rauc is layered — a default build never sees it.)
- Keys: an in-tree **DEV** keypair
  (`dynamic-layers/rauc/recipes-core/rauc/files/dev-ca.{cert,key}.pem`,
  regenerate with `gen-dev-keys.sh`) lets a local build sign+trust its own
  bundle. **DEV ONLY** — production bundles are signed in CI from the
  `RAUC_SIGNING_KEY` secret and the prod cert is baked as the keyring.

## 5. Bundle (.raucb) ✅
- `dynamic-layers/rauc/recipes-core/bundles/update-bundle.bb` (`inherit bundle`)
  produces the signed
  `update-bundle-*.raucb` (`compatible=iot-rpi`, single `rootfs` slot = the
  `iot-image` ext4). The operator drops it on the device-ui (chunk-uploads) or
  the cloud pushes its URL; `iot-swupdate` runs `rauc install`.

## 6. Confirm / rollback ✅ (verify on HW)
- `iot-ota-confirm.service` health-checks the boot and `rauc status mark-good`s
  it; an unhealthy boot is left un-confirmed so the bootloader reverts. The
  boot-attempts/bootcount round-trip is the key thing to validate on hardware.

## 7. CI ✅
- `.github/workflows/rauc-bundle.yml` runs the A/B build (cached) and signs the
  `.raucb` with the `RAUC_SIGNING_KEY` secret on tag/dispatch, publishing the
  bundle + keyring as release assets.

## Acceptance (on hardware)
1. Flash A/B image; boots bank A; `rauc status` shows A good.
2. `rauc install` a B bundle (or drop it in the UI) → writes bank B → reboot → B.
3. `iot-ota-confirm` marks B good; `iot.boot.bank=B`, `iot.boot.confirmed=true`.
4. Install a deliberately-broken bundle → boot fails health check → next boot
   **rolls back to A** automatically.
5. Power-cut mid-`rauc install` → A still boots (inactive-bank write was atomic).
