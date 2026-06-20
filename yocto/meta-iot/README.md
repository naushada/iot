# meta-iot — Yocto Layer for the iot LwM2M Stack

Yocto/OpenEmbedded layer that builds the iot LwM2M 1.1.1 device management
stack and all its third-party dependencies. Seven daemons are delivered as
separate installable packages so embedded images pull only what they need,
plus a full bootable `iot-image` distribution.

## Build steps

No host Yocto install — everything runs in a container. The host needs
only **podman** (or **docker**), ~50 GB free disk, and internet for the
first source fetch. All commands run from the `yocto/` directory.

**1. Build the image.**

```sh
cd yocto
./build.sh                           # raspberrypi3-64 iot-image (default)
# MACHINE=qemuarm64 ./build.sh       # a different machine
# TARGET=packagegroup-iot ./build.sh # just the .ipk feed, no full image
# ./build.sh all                     # every machine in build.sh
```

`build.sh` builds the `iot-yocto-builder` container (Ubuntu 22.04 + poky +
meta-openembedded + meta-raspberrypi, all at Scarthgap 5.0 LTS), runs
`bitbake` inside it, and copies the artifacts back to the host. The **first**
run compiles the whole distribution (toolchain, glibc, ACE, kernel…) — hours.

The build also compiles the **Angular device UI** as part of the iot recipe:
a `do_build_ui` task (runs after `do_patch`, before `do_configure`) uses
`nodejs-native` to `npm ci` + `ng build --configuration production` the SPA
under `iot-ui/`, staging it to `/usr/share/iot/www` in the `iot-httpd`
package (served by `iot-httpd`). The dist is `.gitignore`'d, so it is built
fresh each time rather than fetched. `do_build_ui[network] = "1"` — `npm`
needs network at build time; for a fully offline build, swap to the `npm`
bbclass + an `npmsw://` SRC_URI. Angular 14's webpack needs
`NODE_OPTIONS=--openssl-legacy-provider` on Scarthgap's newer Node, which the
task sets automatically.

**2. Collect the output** — copied back to the host per machine, under
`yocto/build/<machine>/`:

- `images/<machine>/iot-image-<machine>.rootfs-<timestamp>.wic.bz2` —
  flashable SD-card image. A stable `iot-image-<machine>.rootfs.wic.bz2`
  symlink in the same directory always points at the latest build, e.g.
  `yocto/build/raspberrypi3-64/images/raspberrypi3-64/iot-image-raspberrypi3-64.rootfs.wic.bz2`
- `ipk/` — the `.ipk` feed for `opkg install` over ssh

The persistent `iot-yocto-sstate` / `iot-yocto-downloads` volumes make
**reruns incremental** — after the first build, only changed `meta-iot`
recipes recompile (minutes).

**3. (Optional) Publish a cache image** so other machines skip the distro
compile — see *Shareable cache image* below:

```sh
podman login docker.io
CACHE_IMAGE=docker.io/<you>/iot-yocto-builder:cache ./build.sh publish
```

**4. (Optional) Build elsewhere from the cache** — pulls the image and
recompiles only `meta-iot`:

```sh
CACHE_IMAGE=docker.io/<you>/iot-yocto-builder:cache IOT_USE_CACHE=1 ./build.sh
```

**5. Flash + deploy** to the Pi — see *Deploy to a Raspberry Pi 3B* below.

Reset for a **fully clean rebuild** (nuclear — wipes everything):
```sh
podman volume rm iot-yocto-sstate iot-yocto-downloads
```

### Per-recipe clean rebuilds

When only the iot recipe (or its source at `SRCREV`) changes, you can
clean just that recipe without nuking the entire build.  Both commands
use the containerised build — pass bitbake targets through `TARGET`:

```sh
# Re-fetch + re-patch + re-configure iot (keeps downloads, wipes sstate for iot).
# Use when: the recipe .bb, a patch file, or the upstream git source changed.
TARGET="-c cleansstate iot" MACHINE=raspberrypi3-64 ./build.sh

# Nuclear clean — also deletes the fetched git tree under WORKDIR.
# Use when: cleansstate doesn't pick up a new commit (rare with SRCREV=AUTOREV).
TARGET="-c cleanall iot" MACHINE=raspberrypi3-64 ./build.sh
```

After either command, run the normal `./build.sh` to rebuild from the
cleaned state.

**Timing:** Both `cleansstate` and `cleanall` only affect the iot recipe.
The toolchain, kernel, and other 100+ packages remain cached.  Expect
**~10-15 minutes** for the rebuild (104 source files).  Only the very
first `./build.sh` ever takes hours.

**When to use each:**

| Scenario | Command |
|----------|---------|
| Patch file or recipe `.bb` changed | `cleansstate` |
| Source code (CMakeLists.txt, `.cpp`) changed on `main` | `cleansstate` |
| Cleansstate didn't pick up a new commit | `cleanall` |
| Build artifacts corrupted, mysterious errors | `cleanall` |
| Start over from scratch (hours!) | `podman volume rm iot-yocto-sstate iot-yocto-downloads` |

### Shareable cache image (skip the distro compile on a fresh machine)

To get that incremental behaviour on a *different* machine / CI without
rebuilding the toolchain, publish a cache image — the base builder with the
populated sstate + downloads baked in as read-only Yocto mirrors:

```sh
# One-time, after a full ./build.sh has populated the volumes:
podman login docker.io
CACHE_IMAGE=docker.io/<you>/iot-yocto-builder:cache ./build.sh publish

# Then on any machine — pulls the image, restores the toolchain/distro from
# baked sstate, and recompiles ONLY meta-iot (minutes, not hours):
CACHE_IMAGE=docker.io/<you>/iot-yocto-builder:cache IOT_USE_CACHE=1 ./build.sh
```

`publish` snapshots the `iot-yocto-sstate` + `iot-yocto-downloads` volumes
into image layers (via `podman commit`) and pushes. The image is large
(several GB — it carries the whole sstate) and `publish` needs ~2× the
sstate size of free space while it copies. The baked mirrors also insulate
the build from upstream source rug-pulls (a vanished tarball / rebased Git
ref is served from the cache). `entrypoint.sh` auto-detects the baked
mirrors and wires `SSTATE_MIRRORS` / `own-mirrors`.

### Deploy to a Raspberry Pi 3B

```sh
# 1. Flash the SD card. Easiest: the helper script auto-detects the card,
#    wipes it, and writes the latest image (refuses internal/system disks,
#    asks to confirm). Run it from the yocto/ directory:
( cd yocto && ./flash-sd.sh )          # or: ./flash-sd.sh /dev/sdX
#    Manual alternative — find the device (lsblk / `diskutil list`), then dd
#    the whole disk (not a partition); macOS: use the raw node /dev/rdiskN:
bzcat yocto/build/raspberrypi3-64/images/raspberrypi3-64/iot-image-*.wic.bz2 \
  | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress

# 2. Boot the Pi, then ssh in (image runs sshd; debug-tweaks → empty root pw)
ssh root@<pi-ip>

# 3. Push app updates later over ssh via the .ipk feed
scp yocto/build/raspberrypi3-64/ipk/*/iot-*.ipk root@<pi-ip>:/tmp/
ssh root@<pi-ip> 'opkg install /tmp/iot-*.ipk'
```

### OTA updates (LwM2M Object 5)

The manual `scp` + `opkg` step above is for the bench. In the field, app
updates ride the **LwM2M Object 5 (Firmware Update)** flow — the cloud points
the device at an artifact in its firmware feed and the device pulls and applies
it. The artifact is either a **single `.ipk`** or a **`.tar.gz` bundle of every
`iot-*.ipk`** (built by `iot-bundle.bb`) for a whole-userspace upgrade in one
push; the cloud-ui multi-selects endpoints, so one push can target a whole list
of devices. The download runs **direct over the public WAN, not the VPN
tunnel** — the cloud resolves a relative manifest `ipk_url` (`/firmware/...`)
against its public address (`cloud.firmware.base.url`, else the host of
`cloud.dm.uri`), and the `sha256` that pins integrity arrives over the trusted
DTLS control plane, so the payload transport needn't be trusted. This keeps OTA
working when the tunnel is down and lets a bundle safely replace the VPN client
itself. The flow is split into a **stager** and an **inotify-triggered
installer** (full design: `apps/docs/tdd-yocto-swupdate.md`):

- `iot-ota-stage` (`/usr/bin/iot-ota-stage`, in `iot-lwm2m`) is the worker the
  LwM2M client requests on a /5/0/2 execute. The client is **unprivileged**
  (`User=engineer`) and cannot `systemd-run` a system unit, so it drops the URL
  into `/run/iot/update/stage.req`; the **`iot-ota-stage.path`** unit fires
  **`iot-ota-stage.service`** (root), which runs the worker: download the
  artifact (honouring
  `?sha256=` / `?version=` / `?reboot=` params) with **retry + resume**
  (`curl -C -` / `wget -c`, up to `iot.update.retries`, default 5) so a flaky
  uplink doesn't fail the campaign → verify sha256 → for a `.tar.gz` bundle,
  extract the `.ipk`s into the spool (so the installer's `*.ipk` glob applies
  them all) → write to the tmpfs spool `/run/iot/update/` → `touch` the empty
  `update` trigger. It does **not** install.
- `iot-swupdate.path` (systemd **inotify** watch on the trigger) fires
  `iot-swupdate.service` → `/usr/bin/iot-swupdate`, which: snapshots + re-arms,
  `opkg install --force-reinstall --force-downgrade`, runs config/schema
  migrations (§11 below), then **reboots** if a kernel/`base-files`/`u-boot`/
  `systemd` package landed (or `reboot=always`) else `try-restart`s the iot
  daemons. Decoupled from the lwm2m client, so it survives `opkg` replacing the
  running binaries.
- **Config/schema migration:** new schema keys are picked up automatically (the
  persisted store at `/var/lib/iot/data_store.lua` is preserved; schemas in
  `/etc/iot/ds-schemas/` are overwritten by `opkg`). Renames/retypes need a
  migration script under `/usr/share/iot/migrations/NNNN-*.sh`, run once by
  `iot-swupdate` and gated by `iot.config.version`. See that dir's `README.md`.
- Progress is mirrored into the data store (`iot.update.state` / `.result` /
  `.version`), which the relaunched client maps onto Object 5 resources
  (/5/0/3 state, /5/0/5 result, /5/0/7 version) and the device UI reflects.

  | ds key              | values                                              |
  |---------------------|-----------------------------------------------------|
  | `iot.update.state`  | 0 idle · 1 downloading · 2 downloaded · 3 updating  |
  | `iot.update.result` | 0 initial · 1 success · 5 integrity · 8 uri · 9 install |

Re-pushing is idempotent per **campaign**: iot-cloudd stamps each push with a
monotonic `cloud.update.seq` (`cid`), and lwm2m-dm pushes Object 5 at-most-once
per `(endpoint, cid)` — so a fresh push re-sends even an identical version,
while a stuck campaign isn't re-fired every tick.

This covers per-`.ipk` and whole-userspace (`iot-bundle`) updates. Atomic
full-image **A/B** updates (RAUC `.raucb`, behind `IOT_AB=1`) are a separate
path — `iot-swupdate` branches on `.raucb` → `rauc install` + reboot — see
`apps/docs/tdd-ab-image-ota.md`. See `apps/docs/` and the device/cloud OTA
notes for the end-to-end push from the cloud UI.

## What this layer provides

| Recipe                   | Produces                                                    |
|--------------------------|-------------------------------------------------------------|
| `iot_git.bb`             | `PN=iot`; sub-packages: `iot-ds-server`, `iot-ds-cli`, `iot-lwm2m`, `iot-openvpn-client`, `iot-net-router`, `iot-wifi-client`, `iot-httpd`, `iot-config` |
| `images/iot-image.bb`    | Full bootable distribution (`packagegroup-iot-full` + kernel modules + RPi Wi-Fi/BT firmware + sshd + opkg) → `*.wic.bz2` |
| `ace-tao_7.0.0.bb`       | `libACE.so.7.0.0`, `libACE_SSL.so.7.0.0`, dev headers       |
| `tinydtls_git.bb`        | `libtinydtls.a` (static archive). Carries `0002-add-dtls-log-sink.patch` — adds `dtls_set_log_sink()` (present in the in-tree fork `apps/3rdparty/tinydtls`, absent from the legatoproject SRCREV) so the iot binary's DTLS log-sink in `apps/src/main.cpp` links. See the patch header for detail. |
| `mongo-c-driver_1.19.bb` | `libbson-1.0.so`, `libmongoc-1.0.so`                        |
| `mongo-cxx-driver_3.6.bb`| `libbsoncxx.so`, `libmongocxx.so`                            |
| `packagegroup-iot.bb`    | Meta-packages: `packagegroup-iot-core`, `-full`, `-debug`   |

## Package groups

| Group                      | Contents                                                    |
|----------------------------|-------------------------------------------------------------|
| `packagegroup-iot-core`    | ds-server + lwm2m + ds-cli + config (minimal device/fleet)  |
| `packagegroup-iot-full`    | core + httpd + openvpn-client + net-router + wifi-client + runtime deps |
| `packagegroup-iot-debug`   | full + test binaries (when PACKAGECONFIG[gtest] is on)      |

## PACKAGECONFIG options (iot recipe)

| Option   | Default | Effect                                                      |
|----------|---------|-------------------------------------------------------------|
| `mongo`  | ON      | Links mongocxx/bsoncxx/mongoc/bson (~15 MB). RegistryMirror.|
| `gtest`  | OFF     | Builds unit-test binaries.                                  |
| `systemd`| ON      | Installs systemd units + env files. OFF for sysvinit images.|

Override in `local.conf` or kas config:

```
PACKAGECONFIG:remove:pn-iot = "mongo"
```

## systemd unit architecture

The daemons share one Unix socket (`/run/iot/data_store.sock`, created by
ds-server, mode `0660` group `iot`). Three invariants make the set come up
zero-touch; breaking any one of them strands the whole stack (each was a real
bring-up failure — see the git history around PR #212):

- **`/run/iot` is owned solely by `tmpfiles.d/iot.conf`** (`2775 root:iot` —
  setgid + group-writable), **not** by `RuntimeDirectory=iot`. systemd removes
  a unit's `RuntimeDirectory` when it stops, so multiple units sharing
  `RuntimeDirectory=iot` would delete `/run/iot` (and the live socket) whenever
  any of them stopped or crash-looped — and ds-server's own restart would die in
  pre-exec dir setup. **Do not add `RuntimeDirectory=iot` to any unit.**
- **Every daemon that talks to ds-server has `SupplementaryGroups=iot`.** The
  socket is `0660 root:iot`; without group `iot` a `DynamicUser` gets EACCES on
  connect and crash-loops. (lwm2m-client runs as the static `engineer` user,
  also in `iot`.)
- **ds-server owns `/var/lib/iot` via `StateDirectory=iot`** — `tmpfiles` must
  **not** pre-create it, or systemd migrates it `→ /var/lib/private/iot` on
  every boot (DynamicUser state churn).

Auto-enable needs **both** `SYSTEMD_AUTO_ENABLE = "enable"` **and** the shipped
`90-iot.preset`. The image runs `systemctl preset-all` on first boot, which
resets every unit to its preset policy (default *disable*) — without the preset
file the build-time enables are silently undone and units come up
`preset: disabled`. Keep `90-iot.preset` in sync with the `SYSTEMD_AUTO_ENABLE:*`
assignments in `iot_git.bb`.

## Using on a target

Once the `.ipk` packages are installed on the target:

```sh
# Edit env files to point at your LwM2M server
vi /etc/iot/lwm2m-client.env

# Enable and start (full image: already enabled via 90-iot.preset — skip this)
systemctl enable --now iot-ds.service iot-lwm2m-client.service

# Verify
ds-cli --socket=/run/iot/data_store.sock get iot.endpoint
```

## Layer dependencies

- `poky` (scarthgap) — core layer
- `meta-openembedded` (scarthgap) — `meta-oe` (lua, nlohmann-json, gtest),
  `meta-python`, `meta-networking` (openvpn)
- `meta-raspberrypi` (scarthgap) — `raspberrypi3-64` MACHINE, bootfiles,
  `pi-bluetooth`, rpidistro Wi-Fi/BT firmware

`Containerfile` clones all of these into the builder image; `kas-iot.yml`
declares the same set for the kas-driven path.

## Target machines

Default machine is `raspberrypi3-64` (Raspberry Pi 3B, 64-bit). For other
boards or qemu, override `MACHINE`:

```sh
MACHINE=qemux86-64 ./build.sh    # x86-64 qemu (CI)
MACHINE=qemuarm64  ./build.sh    # ARM64 qemu
./build.sh all                   # every machine in build.sh's MACHINES list
```
