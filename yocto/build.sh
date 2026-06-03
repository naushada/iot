#!/bin/bash
# build.sh — Containerised multi-arch Yocto build for the iot LwM2M stack
#
# Builds inside a podman/docker container, then copies artifacts to the
# host. No Yocto install needed on the host — just podman or docker.
#
# Usage:
#   ./build.sh                        # raspberrypi3-64 (default)
#   MACHINE=qemuarm64 ./build.sh      # ARM64 / aarch64 (qemu)
#   MACHINE=qemux86-64 ./build.sh     # x86-64 (qemu, CI)
#   ./build.sh all                    # All supported machines
#
# Default target is the full bootable distribution `iot-image`. Override:
#   TARGET=packagegroup-iot ./build.sh   # build just the .ipk feed
#
# Output per machine:
#   yocto/build/<machine>/images/<machine>/*.wic.bz2   flashable SD image
#   yocto/build/<machine>/ipk/                          *.ipk feed (opkg)
#
# Requirements:
#   - podman or docker
#   - ~50 GB free disk space (Yocto downloads + build + image)
#   - Internet access (first build fetches ~8 GB of sources)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$SCRIPT_DIR/build"
IMAGE_NAME="${IMAGE_NAME:-iot-yocto-builder:latest}"
# bitbake target — the full bootable distribution by default.
TARGET="${TARGET:-iot-image}"
DEFAULT_MACHINE="${MACHINE:-raspberrypi3-64}"

# Supported machines (used by `./build.sh all`)
MACHINES=(
    "raspberrypi3-64"
    "qemuarm64"
    "qemux86-64"
)

# ── Helpers ───────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log_section() { echo -e "\n${GREEN}═══ $1 ═══${NC}"; }
log_info()    { echo -e "${YELLOW} → $1${NC}"; }
log_error()   { echo -e "${RED}ERROR: $1${NC}" >&2; }

# ── Detect container runtime ──────────────────────────────────────────
detect_runtime() {
    if command -v podman &>/dev/null; then
        echo "podman"
    elif command -v docker &>/dev/null; then
        echo "docker"
    else
        log_error "Neither podman nor docker found."
        exit 1
    fi
}

# ── Build the container image ─────────────────────────────────────────
build_image() {
    log_section "Building container image: $IMAGE_NAME"
    $CR build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Containerfile" "$SCRIPT_DIR"
    log_info "Image ready: $IMAGE_NAME"
}

# ── Build one architecture ────────────────────────────────────────────
build_machine() {
    local machine="$1"
    local out="$OUT_DIR/$machine"
    local container="iot-build-${machine//./-}"
    mkdir -p "$out"

    log_section "Building for $machine"

    # Remove any stale container with the same name
    $CR rm -f "$container" &>/dev/null || true

    # Run the build. The container stays around after exit so we can
    # copy artifacts out.
    $CR run --name "$container" \
        -e "MACHINE=$machine" \
        "$IMAGE_NAME" \
        $TARGET

    # Copy artifacts (images/ + ipk/) from the container to the host
    log_info "Copying artifacts to $out ..."
    $CR cp "$container:/home/builduser/yocto/build/tmp/deploy/." "$out/"

    # Clean up the container
    $CR rm "$container"

    log_info "Done: $out/"
}

# ── Print summary ─────────────────────────────────────────────────────
print_summary() {
    log_section "Build summary"
    for machine in "${MACHINES[@]}"; do
        local img
        img=$(find "$OUT_DIR/$machine/images" -name 'iot-image*.wic.bz2' -type f 2>/dev/null | sort | tail -1)
        local ipk_count
        ipk_count=$(find "$OUT_DIR/$machine/ipk" -name 'iot-*.ipk' -type f 2>/dev/null | wc -l | tr -d ' ')
        if [ -n "$img" ]; then
            echo "  ✅ $machine  →  $(basename "$img")  +  ${ipk_count} iot .ipk"
        elif [ "$ipk_count" -gt 0 ]; then
            echo "  ⚠️  $machine  →  ${ipk_count} iot .ipk (no image — package-only build)"
        elif [ -d "$OUT_DIR/$machine" ]; then
            echo "  ⚠️  $machine  →  build completed (no artifacts found)"
        else
            echo "  ⬜ $machine  —  not built"
        fi
    done

    # Concrete next steps for the primary RPi target if it was built.
    local rpi_img
    rpi_img=$(find "$OUT_DIR/raspberrypi3-64/images" -name 'iot-image*.wic.bz2' -type f 2>/dev/null | sort | tail -1)
    if [ -n "$rpi_img" ]; then
        cat <<EOF

  ── Flash the SD card (Raspberry Pi 3B) ──────────────────────────────
    # Find the SD device first (e.g. /dev/sdX on Linux, /dev/diskN on macOS).
    # Fast + verified (recommended, needs bmaptool):
    bmaptool copy "$rpi_img" /dev/sdX
    # Or plain dd:
    bzcat "$rpi_img" | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress

  ── First boot + ssh in ──────────────────────────────────────────────
    # The image runs sshd with debug-tweaks (empty root password).
    ssh root@<pi-ip>          # then: passwd, provision keys for production

  ── Push iot app updates over ssh (opkg feed) ────────────────────────
    scp $OUT_DIR/raspberrypi3-64/ipk/*/iot-*.ipk root@<pi-ip>:/tmp/
    ssh root@<pi-ip> 'opkg install /tmp/iot-*.ipk'
EOF
    fi
    echo ""
}

# ── Main ──────────────────────────────────────────────────────────────
main() {
    CR=$(detect_runtime)
    log_info "Container runtime: $CR"

    build_image

    log_info "Target recipe: $TARGET"
    local machine="${1:-$DEFAULT_MACHINE}"

    if [ "$machine" = "all" ]; then
        for m in "${MACHINES[@]}"; do
            build_machine "$m"
        done
        print_summary
    else
        build_machine "$machine"
        print_summary
    fi
}

main "$@"
