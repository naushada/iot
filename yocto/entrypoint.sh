#!/bin/bash
# entrypoint.sh — run inside the Yocto build container
#
# Sets up the bitbake build directory, adds the required layers,
# writes local.conf, and runs bitbake. All at container run time
# so permissions are correct (no volume-mount conflicts).
#
# Usage:
#   podman run --name iot-build -e MACHINE=qemuarm64 iot-yocto-builder
#   podman cp iot-build:/home/builduser/yocto/build/tmp/deploy ./build/qemuarm64/
#   podman rm iot-build

set -eo pipefail

MACHINE="${MACHINE:-qemux86-64}"
# Default target if none specified — the full bootable distribution.
if [ $# -eq 0 ]; then
    set -- iot-image
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  iot Yocto build"
echo "  Machine:  $MACHINE"
echo "  Targets:  $@"
echo "═══════════════════════════════════════════════════════════════"

cd /home/builduser/yocto

# ── 1. Initialise the build directory ──────────────────────────────
echo "→ Setting up build directory ..."
cd poky
TEMPLATECONF="" . ./oe-init-build-env ../build
cd /home/builduser/yocto/build

# ── 2. Add layers ──────────────────────────────────────────────────
echo "→ Adding layers ..."
bitbake-layers add-layer ../meta-openembedded/meta-oe
bitbake-layers add-layer ../meta-openembedded/meta-python
bitbake-layers add-layer ../meta-openembedded/meta-networking
# meta-raspberrypi provides the raspberrypi3-64 MACHINE, bootfiles,
# pi-bluetooth, and the rpidistro Wi-Fi firmware. Harmless for the qemu
# machines (only its recipes for the selected MACHINE are pulled in).
bitbake-layers add-layer ../meta-raspberrypi
bitbake-layers add-layer ../meta-iot

# ── 3. Configure local.conf ────────────────────────────────────────
cat >> conf/local.conf <<'YOCONF'

# ── iot stack configuration ────────────────────────────────────────

# Accept CLOSED license for iot recipes, plus the Raspberry Pi distro
# Wi-Fi/Bluetooth firmware licenses (synaptics-killswitch gates the
# rpidistro brcm firmware).
LICENSE_FLAGS_ACCEPTED += "CLOSED synaptics-killswitch"

# systemd as init manager (required for DynamicUser=, RuntimeDirectory=)
INIT_MANAGER = "systemd"

# .ipk packages → on-target opkg + a flat ipk feed under tmp/deploy/ipk,
# so iot-*.ipk can be scp'd and `opkg install`ed over ssh. (Poky defaults
# to rpm; without this the ipk feed the scripts reference is never built.)
PACKAGE_CLASSES = "package_ipk"

# Disable mongo PACKAGECONFIG for faster builds (RegistryMirror).
# Remove to enable the MongoDB registration mirror feature.
PACKAGECONFIG:remove:pn-iot = "mongo"

# Filesystem may not support hardlinks (macOS podman host).
# Disable sstate hardlinking to avoid cp -afl failures.
SSTATE_HARDLINK = "0"

# Delete each recipe's WORKDIR right after it builds. Keeps TMPDIR small
# on the constrained container overlay (a full image build is ~50 GB of
# work dirs otherwise). The persistent sstate-cache volume still makes
# reruns fast, so this costs little. The image recipe is auto-excluded.
INHERIT += "rm_work"

# Limit parallelism for constrained VMs (4 GB RAM default).
# Override at run time with -e BB_NUMBER_THREADS=4 and
# -e PARALLEL_MAKE=-j4.
BB_NUMBER_THREADS ?= "2"
PARALLEL_MAKE ?= "-j2"
YOCONF

# ── 4. Override MACHINE ────────────────────────────────────────────
if [ "$MACHINE" != "qemux86-64" ]; then
    echo "MACHINE = \"$MACHINE\"" >> conf/local.conf
fi

# Raspberry Pi tunables: serial console on the GPIO header for headless
# bring-up debugging. (No-op on the qemu machines.)
case "$MACHINE" in
    raspberrypi*)
        cat >> conf/local.conf <<'RPICONF'

# ── Raspberry Pi ───────────────────────────────────────────────────
ENABLE_UART = "1"
RPICONF
        ;;
esac

# ── Baked cache mirrors (present only in a published cache image) ──
# `build.sh publish` bakes a populated sstate-cache + downloads into the
# image at these paths. When present, use them as read-only mirrors so a
# fresh build restores the whole toolchain/distro from sstate and only the
# changed meta-iot recipes recompile.
if [ -d /home/builduser/yocto/sstate-mirror ]; then
    echo '→ Using baked sstate mirror (only changed recipes will rebuild)'
    echo 'SSTATE_MIRRORS ?= "file://.* file:///home/builduser/yocto/sstate-mirror/PATH"' \
        >> conf/local.conf
fi
if [ -d /home/builduser/yocto/dl-mirror ]; then
    echo '→ Using baked downloads mirror'
    cat >> conf/local.conf <<'DLCONF'
SOURCE_MIRROR_URL ?= "file:///home/builduser/yocto/dl-mirror"
INHERIT += "own-mirrors"
DLCONF
fi

# ── 5. Run bitbake ─────────────────────────────────────────────────
echo ""
echo "→ Starting bitbake for $MACHINE: $@ ..."
echo ""

bitbake "$@"

# ── 6. Report ──────────────────────────────────────────────────────
echo ""
echo "═══════════════════════════════════════════════════════════════"
echo "  Build complete — $MACHINE"
echo "  Artifacts: build/tmp/deploy/"
echo "═══════════════════════════════════════════════════════════════"
echo "── SD-card image(s): ──"
find tmp/deploy/images -name '*.wic.bz2' -type f 2>/dev/null | sort || true
echo "── iot .ipk feed: ──"
find tmp/deploy/ipk -name 'iot-*.ipk' -type f 2>/dev/null | sort || true
echo ""
echo "On the host, extract with:"
echo "  podman cp <container>:/home/builduser/yocto/build/tmp/deploy ./build/$MACHINE/"
