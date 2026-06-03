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
# Default targets if none specified
if [ $# -eq 0 ]; then
    set -- packagegroup-iot lwm2m
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
bitbake-layers add-layer ../meta-iot

# ── 3. Configure local.conf ────────────────────────────────────────
cat >> conf/local.conf <<'YOCONF'

# ── iot stack configuration ────────────────────────────────────────

# Accept CLOSED license for iot recipes
LICENSE_FLAGS_ACCEPTED += "CLOSED"

# systemd as init manager (required for DynamicUser=, RuntimeDirectory=)
INIT_MANAGER = "systemd"

# Target packages
IMAGE_INSTALL:append = " packagegroup-iot-core"

# Disable mongo PACKAGECONFIG for faster builds (RegistryMirror).
# Remove to enable the MongoDB registration mirror feature.
PACKAGECONFIG:remove:pn-lwm2m = "mongo"

# Filesystem may not support hardlinks (macOS podman host).
# Disable sstate hardlinking to avoid cp -afl failures.
SSTATE_HARDLINK = "0"

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
find tmp/deploy/ipk -name 'iot-*.ipk' -type f 2>/dev/null | sort || true
echo ""
echo "On the host, extract with:"
echo "  podman cp <container>:/home/builduser/yocto/build/tmp/deploy ./build/$MACHINE/"
