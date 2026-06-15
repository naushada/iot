# RAUC A/B image OTA — device bring-up checklist

Status: **scaffold — not yet validated on hardware.** This lists the Yocto/boot
work to turn the Phase-2 design (`apps/docs/tdd-ab-image-ota.md`) into a booting
A/B image. The runtime hooks (chunked `.raucb` upload, `iot-swupdate` → `rauc
install` routing, `iot-ota-confirm` mark-good/rollback, the `iot.boot.*` ds keys,
device-ui bank status) are already in the tree; the items below are the
build/bootloader pieces that need a Yocto build + an RPi to validate.

## 1. Layers
- Add **`meta-rauc`** and **`meta-rauc-community`** (RPi support) to `bblayers`
  (kas-iot.yml / the build container).

## 2. Bootloader (u-boot)
- Switch the RPi to **u-boot** (`PREFERRED_PROVIDER_virtual/bootloader = "u-boot"`,
  `RPI_USE_U_BOOT = "1"`).
- Enable the RAUC u-boot boot-select integration (`rauc-uboot` / the community
  layer's `BOOT_ORDER` + `bootcount` env), with `boot-attempts` matching
  `system.conf`.

## 3. Image
- `WKS_FILE = "iot-ab.wks.in"` (the 4-partition layout in `yocto/meta-iot/wic/`).
- `IMAGE_FSTYPES += "wic.bz2"` for the SD image, and the RAUC **bundle** image
  type for the `.raucb` (`IMAGE_CLASSES += "rauc"` / a `bundle.bb`).
- Mount the data partition at `/var/lib/iot` (fstab/systemd mount); keep ds
  state + certs there so an image swap never loses them.

## 4. RAUC config + keys
- Install `recipes-core/rauc/files/system.conf` → `/etc/rauc/system.conf`
  (slot devices must match the wks partitions).
- Generate a signing keypair; bake the **cert** into the image at
  `/etc/rauc/keyring.pem`; keep the **key** as a CI secret.
- `set compatible=iot-rpi` to match the bundle manifest.

## 5. Bundle (.raucb)
- A `rauc-bundle` recipe producing the signed `.raucb` (rootfs image + manifest,
  `compatible=iot-rpi`).
- The operator drops the `.raucb` on the device-ui (already chunk-uploads) or the
  cloud pushes its URL; `iot-swupdate` runs `rauc install` (already routed).

## 6. Confirm / rollback
- `iot-ota-confirm.service` (already in the recipe) health-checks the boot and
  `rauc status mark-good`s it; an unhealthy boot is left un-confirmed so the
  bootloader reverts. Verify the boot-attempts/bootcount round-trip on HW.

## 7. CI
- A workflow (like `cloud-image.yml`) to build + **sign** the `.raucb` release
  artifact on tag, using the CI signing-key secret.

## Acceptance (on hardware)
1. Flash A/B image; boots bank A; `rauc status` shows A good.
2. `rauc install` a B bundle (or drop it in the UI) → writes bank B → reboot → B.
3. `iot-ota-confirm` marks B good; `iot.boot.bank=B`, `iot.boot.confirmed=true`.
4. Install a deliberately-broken bundle → boot fails health check → next boot
   **rolls back to A** automatically.
5. Power-cut mid-`rauc install` → A still boots (inactive-bank write was atomic).
