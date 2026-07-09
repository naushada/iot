# TDD — Phase-2 A/B (dual-bank) image OTA with rollback

Status: **WIRED — pending HW validation** (RAUC) · Target: Yocto/RPi
· Author: 2026-06-14

**Increment 1 (✅ shipped):** chunked-append upload (handles large `.raucb`),
`iot-swupdate` `.raucb`→`rauc` routing (guarded), device-ui `.raucb` drop.
**Increment 2 (✅ wired, build/bootloader — §§2–4, 7):** `meta-rauc` +
`meta-lts-mixins` (scarthgap/u-boot) + `meta-rauc-raspberrypi` layers, u-boot
boot-select, `WKS_FILE=iot-ab.wks.in`
(4-partition wic), `rauc_%.bbappend` (system.conf + keyring), `update-bundle.bb`
(signed `.raucb`), dev/prod signing keys, and the `rauc-bundle.yml` CI workflow —
all behind the `IOT_AB=1` build switch (default build stays single-rootfs). See
`yocto/meta-iot/docs/rauc-bringup.md`. **Remaining: hardware validation only** —
the boot-attempts/bootcount rollback round-trip on a real RPi (the Acceptance
checklist in rauc-bringup.md).

## 1. Why (what Phase 1 can't do)

Phase-1 OTA (`apps/docs/tdd-yocto-swupdate.md`) installs **`.ipk` packages with
`opkg`, in place, on a single rootfs**. That means:

- **No atomicity** — a power loss mid-`opkg install` can leave a half-written,
  inconsistent rootfs (partial files + a corrupt package DB).
- **No rollback** — there is no previous image to fall back to; on reboot the
  device boots the *same* (possibly broken) rootfs.
- **No full-image / kernel+rootfs swap** — only userspace package updates.

Phase 2 adds **A/B (dual-bank) image OTA**: write a full image to the *inactive*
bank, atomically switch the bootloader to it, health-check on first boot, and
**auto-revert to the previous bank if the new one fails**. This is the
power-fail-safe, rollback-capable path — and the home for the operator's
"full bundle" drag-and-drop.

Phase 1 (opkg) and Phase 2 (image) **coexist**: opkg for quick userspace tweaks;
A/B image for kernel/rootfs/coordinated releases.

## 2. Layout (Raspberry Pi 3B reference)

Dual rootfs banks + shared persistent data, selected by the bootloader:

```
mmcblk0p1  boot   (FAT) — bootloader + per-bank kernel/dtb, boot-select env
mmcblk0p2  rootA  (ext4, ro) — bank A rootfs
mmcblk0p3  rootB  (ext4, ro) — bank B rootfs
mmcblk0p4  data   (ext4, rw) — /var/lib/private (ds StateDirectory), persists
```

- Only the **inactive** bank is written during an update; the running bank is
  never touched → atomic + power-fail-safe (an interrupted write just leaves the
  inactive bank invalid; the active bank still boots).
- **Persistent data is a separate partition** so an image swap never loses ds
  state / credentials / the `iot.config.version` migration marker. The §11
  config-migration step (Phase-1 doc) still runs after a bank switch when the
  schema generation changed.

### 2.1 Persisting ds config across a bank swap (the "OTA → offline at 90%" fix)

**Field failure (2026-07):** a full-image OTA left the device with an empty data
store — WiFi/PSKs gone, so it booted offline, never re-registered, and the cloud
OTA sat at **90% forever** (the cloud's optimistic phase; 100% only arrives when
the device re-registers on the new version). Root cause: iot-ds runs
`DynamicUser=yes` + `StateDirectory=iot`, so its `data_store.lua` (which holds
**all** operator config) actually lives at **`/var/lib/private/iot/`** — on the
**rootfs**. The `data` partition existed but was **mounted nowhere**, so a RAUC
bank swap (fresh rootfs on the inactive bank) started with an empty ds. The
"persistent data partition" above was aspirational, not wired.

**Fix:** ship `var-lib-private.mount` (recipes-iot/lwm2m) — a systemd mount unit
that mounts `LABEL=data` at **`/var/lib/private`**, so the DynamicUser state dir
lands on the persistent partition and survives a bank swap.

- **`/var/lib/private`, never `/var/lib/iot`** — a mount at the latter collides
  with the StateDirectory=iot symlink and crash-loops iot-ds (238/STATE_DIRECTORY,
  HW-confirmed, the earlier revert PR).
- **Self-scoping** via `ConditionPathExists=/dev/disk/by-label/data`: the default
  single-rootfs image has no `data` partition, so the unit skips instantly (no
  device-timeout, no boot delay) — safe to ship in every image.
- **Ordering:** `iot-ds.service` gains `After=var-lib-private.mount` (ordering
  only, not `Requires=`), so it never opens `data_store.lua` before the partition
  is mounted, and is unaffected on the single-rootfs image where the mount skips.
- **Perms:** tmpfiles `z /var/lib/private 0700 root root` fixes the fresh ext4
  root (0755) to what systemd's DynamicUser machinery expects, before ds starts.
- **Preset:** `enable var-lib-private.mount` in `90-iot.preset` so first-boot
  `preset-all` doesn't drop the `WantedBy=local-fs.target` symlink.
- **Bonus:** `rauc.status` (`statusfile=/var/lib/iot/rauc.status`) now persists
  too — `/var/lib/iot` → `/var/lib/private/iot`, on the partition.

**Transition (one-time):** existing devices have an empty `data` partition, so
the first boot after this change re-seeds ds once (re-enter WiFi one final time),
then it is durable. A **reflash** (creates/formats p4 fresh) is cleanest. To lose
nothing across the cutover, before OTA-ing to the fixed image copy the live store
onto the partition on the running device:
```sh
mkdir -p /mnt/data && mount /dev/mmcblk0p4 /mnt/data
cp -a /var/lib/private/iot /mnt/data/     # → /mnt/data/iot/{data_store.lua,.seeded,…}
sync && umount /mnt/data
# then OTA; the fixed image mounts LABEL=data at /var/lib/private → config present.
# (iot-ds's DynamicUser StateDirectory re-chowns /var/lib/private/iot on start,
#  so copying as root is fine.)
```

## 3. Update agent — decision needed

| Option | Pros | Cons |
|--------|------|------|
| **RAUC** (recommended) | Purpose-built A/B, strong Yocto layer (`meta-rauc`, `meta-rauc-community` RPi), bundle signing (X.509), simple `rauc install <bundle>.raucb`, bootloader-agnostic (u-boot/grub/barebox) | adds u-boot (RPi default uses its own loader) |
| **Mender** | Full fleet mgmt + server | heavier; server-centric; we already have our own cloud plane |
| **SWUpdate** (the real one) | Flexible, A/B via `sw-description` | more wiring; overlaps RAUC |

**Recommendation: RAUC.** It's the least-glue A/B engine, signs bundles, and has
maintained RPi Yocto integration. The "full bundle" = a **`.raucb`** (signed
squashfs of the rootfs image + manifest). Open decision in §8.

## 4. Boot-select + rollback flow

1. Bootloader reads a small **boot-select env** (u-boot `bootcount` + `BOOT_ORDER`
   / RAUC's `rauc-grub`/`rauc-uboot` integration): try bank X, increment a
   per-bank `tries` counter.
2. If `tries` exceeds the limit (boot looped / never confirmed) → bootloader
   **switches to the other bank** automatically (rollback).
3. After a successful boot, a **health check** (systemd
   `iot-ota-confirm.service`: ds reachable + iot-httpd up + lwm2m registered
   within N min) marks the bank **good** (`rauc status mark-good`), resetting the
   try counter. Until confirmed, a reboot reverts.

So a bad/incomplete image **never sticks**: either the write was interrupted
(inactive bank invalid → active bank keeps running) or the new bank boots badly
(health check fails → next boot rolls back).

## 5. Bundle delivery (URL + drag-and-drop)

Mirrors Phase-1's two sources, but the artifact is a `.raucb` and the installer
is `rauc`, not `opkg`:

- **Cloud push** — `iot.update.request` carries a `.raucb` URL; a Phase-2 stager
  variant pulls + verifies signature, then `rauc install`.
- **Drag-and-drop (device-ui)** — the operator drops a `.raucb`. A full bundle is
  **tens–hundreds of MB**, so the upload is **chunked-append** (✅ shipped,
  increment 1): the UI slices the file into ≤8 MiB chunks and POSTs them
  sequentially to `POST /api/v1/update/upload?name&offset&final`; httpd appends
  each chunk to the spool file (no full-buffer, stays under the body cap) and
  trips the installer on the final chunk. `iot-swupdate` routes `.raucb` →
  `rauc install` (vs `.ipk` → `opkg`) — ✅ shipped (guarded by `command -v rauc`,
  so it's inert until the RAUC image lands).
- What's **still needed** (device bring-up): `rauc install <file>.raucb`
  (signature-verified) writing the inactive bank, reboot, health-check confirm /
  bootloader rollback — i.e. everything in §3/§4/§7 below.

## 6. State / UI

Reuse the OTA ds keys, extended:
- `iot.update.state`: add `4 writing-bank`, `5 awaiting-confirm`.
- `iot.update.result`: add `2 rolled-back`.
- `iot.boot.bank` (A/B), `iot.boot.confirmed` (bool) for the UI to show which
  bank is running and whether it's confirmed.
- device-ui: the same software-update page shows bank + confirm status, and the
  drag-zone accepts `.ipk` (Phase 1, opkg) **or** `.raucb` (Phase 2, A/B) — it
  routes by extension.

### 6.1 Both-banks view + manual bank switch (shipped)

The Software page shows **both** banks — bootname, installed version, and
good/running status — and an operator can switch the active bank from there.

- **`iot.boot.banks`** — JSON array `[{bootname,slot,version,state,booted}, …]`,
  published by `iot-ota-confirm` at boot (and re-emitted after `mark-good`). It
  sources `rauc status --output-format=shell`; the booted bank's version is the
  authoritative `iot.version`, the other bank's comes from its persisted slot
  status — which is why `system.conf` now sets a central
  `statusfile=/var/lib/iot/rauc.status` on the shared data partition (so the
  booted bank can read the inactive bank's `bundle.version`).
- **`iot.boot.switch.request`** — the device-ui writes the **target bootname**
  ("A"/"B", or "other"). The lwm2m client watches it and launches
  `iot-bank-switch` as root (via `systemd-run`, the same unprivileged→root
  bridge as `iot-ota-stage`); it `rauc status mark-active`s the target slot,
  clears the key, and reboots.
- **Rollback is automatic and needs no UI:** the bootloader's `boot-attempts=3`
  reverts to the previous bank if the switched-to bank never reaches a healthy
  `mark-good` (`iot-ota-confirm`). The UI surfaces this in the confirm dialog.
- Files: `iot-bank-switch` (script), `iot-ota-confirm` (publishes banks),
  `apps/src/main.cpp` (watch), `iot.lua` (keys), `system.conf` (statusfile),
  `iot-ui/.../software-update.component.ts` (banks table + switch modal).

## 7. Yocto work (sketch)

- Add `meta-rauc` (+ RPi community layer); switch RPi to u-boot (or use the
  RAUC RPi boot integration).
- `wic` image with the 4-partition layout (2 rootfs banks + boot + data).
- A RAUC system.conf (slots A/B), bundle signing keys (CI signs `.raucb`).
- `iot-ota-confirm.service` health-check (mark-good).
- A Phase-2 stager/installer (`rauc install`) alongside the opkg `iot-swupdate`.
- CI: build + sign the `.raucb` release artifact (new workflow, like
  `cloud-image.yml`).

## 8. Open decisions

1. **Agent = RAUC** (recommended) vs Mender vs SWUpdate.
2. **Bootloader** — move RPi to u-boot (needed by most A/B integrations) vs the
   RPi native loader + a custom boot-select. (u-boot recommended.)
3. **Partition sizing** — bank size (2× rootfs) vs SD capacity; data partition
   size.
4. **Bundle signing/keys** — where the signing key lives (CI secret) + how the
   device trusts it (baked CA).
5. **Coexistence policy** — when to use opkg (Phase 1) vs A/B (Phase 2): e.g.
   userspace-only → opkg; kernel/base/coordinated → A/B.
6. **Migration** — does a bank switch re-run the §11 config migrations (yes, on
   schema-generation change), and how rollback interacts with an already-applied
   migration (forward-only migrations + a data-partition snapshot?).

## 9. Relationship to Phase 1

| | Phase 1 (shipped) | Phase 2 (this doc) |
|---|---|---|
| Artifact | `.ipk` (or multi-`.ipk` bundle) | `.raucb` full image |
| Installer | `opkg` (in place) | `rauc` (inactive bank) |
| Atomic / power-fail-safe | ❌ | ✅ |
| Rollback | ❌ | ✅ (bootloader + health check) |
| Upload | raw body ≤ 8 MiB | streaming to disk |
| Scope | userspace daemons | kernel + rootfs |

## 10. Build & flash workflow (+ the `/dev/sda4` self-brick recovery)

### Build the A/B image
```sh
IOT_AB=1 ./build.sh                 # u-boot + 4-partition wic + signed .raucb
```
Artifacts land on the host at `yocto/build/<machine>/images/<machine>/`. The
orchestrated `build.sh` path auto-copies them out of the build container.

### The `/dev/sda4` (and `mmcblk0p5/6`) self-brick
A baked `/etc/fstab` that references a partition the target lacks makes systemd's
`local-fs.target` time out (~90 s) and drop to **emergency mode** on first boot —
`(1 of 2) A start job is running for /dev/sda4`. On A/B this bricks **both** banks
before rollback can help.

**TRUE root cause (HW-confirmed by extracting the wic's fstab): `wic`'s
`--fstab-update`.** wic appends fstab entries for any partition lacking
`--no-fstab-update`, using **`/dev/sdaN`** device names — which don't exist on an
mmc-booted RPi. The A/B wks's boot partition (and, via a stale `do_image_wic`, the
data partition) got `/dev/sda1 /boot` and `/dev/sda4 /var/lib/iot` appended *after*
the clean base-files fstab. This is injected at `do_image_wic`, **after** do_rootfs
and after the `iot-image.bb` fstab precheck (which only sees the *rootfs* fstab) —
so it ships silently. The base-files ipk is clean the whole time; `cleansstate
base-files` does NOT fix it. (A genuinely stale `do_rootfs` carrying an OLD
base-files fstab is a *different*, rarer path — that one `cleansstate` does fix.)

**Fix (PR — `iot-ab.wks.in`):** `--no-fstab-update` on **every** partition, so
base-files is the sole fstab authority and wic never writes `/dev/sda*`. Changing
the wks also invalidates the stale `do_image_wic`.

**Guard:** `iot-image.bb`'s `fstab_sanity_check` runs at `ROOTFS_POSTPROCESS_COMMAND`
and `IMAGE_POSTPROCESS_COMMAND` (#462) — but note it scans the **rootfs** fstab, so
it catches a stale base-files but NOT a wic `--fstab-update` injection; the wks
`--no-fstab-update` is what closes that hole. To verify a built `.wic.bz2`, mount
rootA and read its fstab: `file img.wic` for the p2 start sector, then
`mount -o loop,ro,offset=<sector*512> img.wic /mnt && cat /mnt/etc/fstab`.

**Fix a genuinely stale rootfs sstate (the other path) — rebuild from clean:**
```sh
IOT_CLEAN=1 ./build.sh              # cleansstate base-files + iot-image, then build+copy
# (equivalent manual form, inside `./build.sh shell`:)
#   bitbake -c cleansstate base-files iot-image && bitbake iot-image
```

### Getting a `shell`-built image onto the host
`./build.sh shell` runs `--rm` and does **not** auto-copy. A manual `bitbake`
there strands the image in the container (lost on exit) — reflashing then grabs
the OLD host image and the boot hang persists. Pull it out from a 2nd terminal:
```sh
./build.sh copy [machine]          # podman/docker cp deploy/ → yocto/build/<machine>/
```

### Flash
```sh
./yocto/flash-sd.sh --wifi-ssid <SSID> --wifi-psk <PSK>   # newest image via the stable symlink
```
`flash-sd.sh` follows `iot-image-<machine>.rootfs.wic.bz2` (→ newest build) and
seeds WiFi onto the FAT boot partition (the SD device is auto-detected, or pass
`/dev/sdX`/`/dev/diskN`). **Confirm the resolved image is the one you just built**
(check its timestamp) — a stale symlink or an un-copied build is the usual cause
of "I flashed the latest but still see `/dev/sda4`". On the booted device:
`grep sda /etc/fstab` (expect none) and `systemctl is-active iot-ds` (expect
`active`).

### Update the userspace WITHOUT reflashing (OTA via the cloud UI)
For an iot-app-only change (no kernel/base/unit change), don't rebuild the whole
image or reflash — build just the iot userspace and push it over LwM2M from the
cloud-ui **Software Update** page. Deep design: `tdd-yocto-swupdate.md`.

**1. Build only the iot bundle** (recompiles just the `iot` recipe; deps restore
from sstate, so minutes not hours):
```sh
TARGET=iot-bundle ./build.sh raspberrypi3-64
# → yocto/build/raspberrypi3-64/images/raspberrypi3-64/
#     iot-bundle-<PV>-<machine>.tar.gz          (every iot-*.ipk, tarred flat)
#     iot-bundle-<PV>-<machine>.tar.gz.manifest.json   (paste-ready catalogue row)
#     iot-bundle-<PV>-<machine>.tar.gz.sha256
# (TARGET=iot → just the .ipk feed for an `opkg install` over ssh instead.)
```

**2. Put it in the cloud firmware feed.** Copy the tarball into the cloud's
`iot-firmware` volume (served at `/firmware/<name>`), then add its manifest row to
the `cloud.firmware.manifest` ds array — easiest via the Software Update page's
"add firmware" form; or paste the `.manifest.json` contents manually:
```sh
scp .../iot-bundle-*.tar.gz* <cloud>:/firmware/
# manifest row: { "pkg":"iot-bundle", "version":"<PV>", "arch":"<machine>",
#                 "ipk_url":"/firmware/<name>", "sha256":"<sha>" }
```

**3. Push from the UI.** cloud-ui → **Software Update**: pick the package, tick the
target device(s) in the grid, **Push** (Admin only). The cloud WRITEs LwM2M Object
5 `/5/0/1` + EXECUTEs `/5/0/2`; the device downloads + sha256-verifies, then
`opkg install`s the bundle. Progress streams live (`cloud.update.status`).

Gotchas (HW-hit): `cloud.firmware.base.url` must point
at the **https** feed (the device can't reach a dead `http:80`); the manifest
`sha256` must have no embedded newline; and the device's update stager runs as
root (an engineer-role user can't `systemd-run` it).
