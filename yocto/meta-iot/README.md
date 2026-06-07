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
# 1. Flash the SD card. Find the device first: lsblk (Linux) or
#    `diskutil list` (macOS) — write to the whole disk, not a partition.
#    Decompress on the fly and write with dd:
bzcat yocto/build/raspberrypi3-64/images/raspberrypi3-64/iot-image-*.wic.bz2 \
  | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
#    (Linux: /dev/sdX. macOS: /dev/rdiskN — the raw node is much faster.)

# 2. Boot the Pi, then ssh in (image runs sshd; debug-tweaks → empty root pw)
ssh root@<pi-ip>

# 3. Push app updates later over ssh via the .ipk feed
scp yocto/build/raspberrypi3-64/ipk/*/iot-*.ipk root@<pi-ip>:/tmp/
ssh root@<pi-ip> 'opkg install /tmp/iot-*.ipk'
```

## What this layer provides

| Recipe                   | Produces                                                    |
|--------------------------|-------------------------------------------------------------|
| `iot_git.bb`             | `PN=iot`; sub-packages: `iot-ds-server`, `iot-ds-cli`, `iot-lwm2m`, `iot-openvpn-client`, `iot-net-router`, `iot-wifi-client`, `iot-httpd`, `iot-config` |
| `images/iot-image.bb`    | Full bootable distribution (`packagegroup-iot-full` + kernel modules + RPi Wi-Fi/BT firmware + sshd + opkg) → `*.wic.bz2` |
| `ace-tao_7.0.0.bb`       | `libACE.so.7.0.0`, `libACE_SSL.so.7.0.0`, dev headers       |
| `tinydtls_git.bb`        | `libtinydtls.a` (static archive)                             |
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

## Using on a target

Once the `.ipk` packages are installed on the target:

```sh
# Edit env files to point at your LwM2M server
vi /etc/iot/lwm2m-client.env

# Enable and start
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
