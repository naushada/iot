#!/bin/bash
# entrypoint.sh — run inside the Yocto build container
#
# Sources the bitbake environment and runs the build. MACHINE can be
# set via the environment at `podman run` time. Default: qemux86-64.
#
# Usage:
#   podman run -e MACHINE=qemuarm64 iot-yocto-builder
#   podman run -e MACHINE=qemuarm64 iot-yocto-builder packagegroup-iot-full

set -euo pipefail

MACHINE="${MACHINE:-qemux86-64}"
TARGET="${1:-packagegroup-iot-core}"

echo "═══════════════════════════════════════════════════════════════"
echo "  iot Yocto build"
echo "  Machine:  $MACHINE"
echo "  Target:   $TARGET"
echo "═══════════════════════════════════════════════════════════════"

cd /home/builduser/yocto

# Source the bitbake build environment from the poky directory
# shellcheck disable=SC1091
cd /home/builduser/yocto/poky
. ./oe-init-build-env ../build

# Override MACHINE in local.conf if passed via env
if [ "$MACHINE" != "qemux86-64" ]; then
    echo "MACHINE = \"$MACHINE\"" >> conf/local.conf
fi

echo ""
echo "→ Starting bitbake $TARGET ..."
echo ""

bitbake "$TARGET"

echo ""
echo "═══════════════════════════════════════════════════════════════"
echo "  Build complete"
echo "  Artifacts: tmp/deploy/ipk/"
echo "═══════════════════════════════════════════════════════════════"

# List the built iot packages
find tmp/deploy/ipk -name 'iot-*.ipk' -type f 2>/dev/null | sort || true
