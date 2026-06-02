#!/bin/bash
# build.sh — Containerised multi-arch Yocto build for the iot LwM2M stack
#
# Builds the entire iot stack inside a podman/docker container. The host
# needs only podman or docker — no Yocto install, no kas, nothing else.
#
# Usage:
#   ./build.sh                        # Build default machine (qemux86-64)
#   MACHINE=qemuarm64 ./build.sh      # Build for ARM64 / aarch64
#   MACHINE=qemuarm ./build.sh        # Build for ARMv7 / armhf
#   ./build.sh all                    # Build all supported architectures
#
# Output per machine:
#   yocto/build/<machine>/ipk/        *.ipk packages (opkg-installable)
#   yocto/build/<machine>/images/     rootfs / kernel images
#
# Requirements:
#   - podman or docker
#   - ~30 GB free disk space (Yocto builds are large)
#   - Internet access (first build fetches ~8 GB of sources)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
IMAGE_NAME="${IMAGE_NAME:-iot-yocto-builder:latest}"
CONTAINER_CMD="${CONTAINER_CMD:-podman}"

# Supported architectures
MACHINES=(
    "qemux86-64"
    "qemuarm64"
    "qemuarm"
)

# ── Helpers ───────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log_section() { echo -e "\n${GREEN}═══ $1 ═══${NC}"; }
log_info()    { echo -e "${YELLOW} → $1${NC}"; }
log_error()   { echo -e "${RED}ERROR: $1${NC}" >&2; }

# ── Detect container runtime ──────────────────────────────────────────
detect_runtime() {
    if command -v podman &>/dev/null; then
        CONTAINER_CMD="podman"
    elif command -v docker &>/dev/null; then
        CONTAINER_CMD="docker"
    else
        log_error "Neither podman nor docker found. Install one to continue."
        exit 1
    fi
    log_info "Container runtime: $CONTAINER_CMD"
}

# ── Build the container image (one time) ──────────────────────────────
build_image() {
    log_section "Building container image: $IMAGE_NAME"
    $CONTAINER_CMD build \
        -t "$IMAGE_NAME" \
        -f "$SCRIPT_DIR/Containerfile" \
        "$SCRIPT_DIR"
    log_info "Image ready: $IMAGE_NAME"
}

# ── Run the build inside the container ────────────────────────────────
run_build() {
    local machine="$1"
    local out_dir="$BUILD_DIR/$machine"
    mkdir -p "$out_dir"

    log_section "Building for $machine"

    # Run the container. Mount the per-machine output directory so
    # deploy artifacts (ipk/, images/) land on the host.
    $CONTAINER_CMD run --rm \
        -e "MACHINE=$machine" \
        -v "$out_dir:/home/builduser/yocto/build/tmp/deploy:Z" \
        "$IMAGE_NAME" \
        packagegroup-iot-core

    log_info "Artifacts: $out_dir/"
}

# ── Print summary ─────────────────────────────────────────────────────
print_summary() {
    log_section "Build summary"
    for machine in "${MACHINES[@]}"; do
        local d="$BUILD_DIR/$machine/ipk"
        if [ -d "$d" ]; then
            local iot_count=$(find "$d" -name 'iot-*.ipk' -type f 2>/dev/null | wc -l)
            local total=$(find "$d" -name '*.ipk' -type f 2>/dev/null | wc -l)
            if [ "$iot_count" -gt 0 ]; then
                echo "  ✅ $machine  →  $iot_count iot .ipk packages ($total total)"
            else
                echo "  ⚠️  $machine  →  build completed (no iot .ipk found yet)"
            fi
        else
            echo "  ⬜ $machine  —  not built"
        fi
    done
    echo ""
    echo "  Install on target (example for ARM64):"
    echo "    scp $BUILD_DIR/qemuarm64/ipk/*/iot-*.ipk root@<target>:/tmp/"
    echo "    ssh root@<target> opkg install /tmp/iot-ds-server_*.ipk /tmp/iot-lwm2m_*.ipk /tmp/iot-config_*.ipk"
    echo ""
}

# ── Main ──────────────────────────────────────────────────────────────
main() {
    detect_runtime
    build_image

    local target="${1:-${MACHINE:-qemux86-64}}"

    if [ "$target" = "all" ]; then
        for machine in "${MACHINES[@]}"; do
            run_build "$machine"
        done
        print_summary
    else
        run_build "$target"
        print_summary
    fi
}

main "$@"
