# TDD — Phase-2 A/B (dual-bank) image OTA with rollback

Status: **DESIGN (for review)** · Target: Yocto/RPi · Author: 2026-06-14

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
mmcblk0p4  data   (ext4, rw) — /var/lib/iot (ds store), /etc/iot overrides, certs
```

- Only the **inactive** bank is written during an update; the running bank is
  never touched → atomic + power-fail-safe (an interrupted write just leaves the
  inactive bank invalid; the active bank still boots).
- **Persistent data is a separate partition** so an image swap never loses ds
  state / credentials / the `iot.config.version` migration marker. The §11
  config-migration step (Phase-1 doc) still runs after a bank switch when the
  schema generation changed.

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
- **Drag-and-drop (device-ui)** — the operator drops a `.raucb`. Because a full
  bundle is **tens–hundreds of MB**, this needs a **streaming upload** (the
  Phase-1 raw-body endpoint buffers in memory, capped at 8 MiB — unsuitable):
  - `POST /api/v1/update/upload` streams the body **to disk** on the data
    partition (chunked write, no full-buffer), reporting progress.
  - Then `rauc install <file>.raucb` (signature-verified) writes the inactive
    bank; reboot into it; health-check confirms or rolls back.

Streaming upload is a prerequisite and is **out of scope for Phase 1** (which
keeps the 8 MiB in-memory cap for `.ipk`).

## 6. State / UI

Reuse the OTA ds keys, extended:
- `iot.update.state`: add `4 writing-bank`, `5 awaiting-confirm`.
- `iot.update.result`: add `2 rolled-back`.
- `iot.boot.bank` (A/B), `iot.boot.confirmed` (bool) for the UI to show which
  bank is running and whether it's confirmed.
- device-ui: the same software-update page shows bank + confirm status, and the
  drag-zone accepts `.ipk` (Phase 1, opkg) **or** `.raucb` (Phase 2, A/B) — it
  routes by extension.

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
