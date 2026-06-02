#!/bin/bash
# build.sh — Containerised Yocto build for the iot LwM2M stack
#
# Runs the entire Yocto build inside the ghcr.io/siemens/kas/kas
# container image. The host needs only podman or docker — no native
# Yocto install, no kas CLI.
#
# Usage:
#   ./build.sh                      # Build default machine (qemux86-64)
#   MACHINE=qemuarm64 ./build.sh    # Build for ARM64
#   MACHINE=raspberrypi4-64 ./build.sh  # Build for Raspberry Pi 4
#   ./build.sh all                  # Build all supported architectures
#
# Output:
#   yocto/build/deploy/ipk/         *.ipk packages (installable on target)
#   yocto/build/deploy/images/      rootfs images
#
# Requirements:
#   - podman or docker
#   - Internet access (to fetch kas image + Yocto layers)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
KAS_IMAGE="${KAS_IMAGE:-ghcr.io/siemens/kas/kas:latest}"
CONTAINER_CMD="${CONTAINER_CMD:-podman}"

# Supported architectures — mirrored in kas-iot.yml
MACHINES=(
    "qemux86-64"
    "qemuarm64"
    "qemuarm"
)

# ── Colour helpers ──────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Colour

log_section() { echo -e "\n${GREEN}═══ $1 ═══${NC}"; }
log_info()    { echo -e "${YELLOW} → $1${NC}"; }
log_error()   { echo -e "${RED}ERROR: $1${NC}" >&2; }

# ── Detect container runtime ────────────────────────────────────────
detect_runtime() {
    if command -v podman &>/dev/null; then
        CONTAINER_CMD="podman"
    elif command -v docker &>/dev/null; then
        CONTAINER_CMD="docker"
    else
        log_error "Neither podman nor docker found. Install one to continue."
        exit 1
    fi
    log_info "Using container runtime: $CONTAINER_CMD"
}

# ── Pull kas image ──────────────────────────────────────────────────
pull_kas() {
    log_section "Pulling kas container image"
    $CONTAINER_CMD pull "$KAS_IMAGE"
}

# ── Build one machine ───────────────────────────────────────────────
build_machine() {
    local machine="$1"
    local deploy_dir="$BUILD_DIR/$machine"

    log_section "Building for $machine"
    mkdir -p "$deploy_dir"

    # kas-container runs kas inside the container. We bind-mount:
    #   - the repo root at /work (so kas finds yocto/kas-iot.yml)
    #   - a per-machine build dir so caches don't collide
    $CONTAINER_CMD run --rm \
        -v "$REPO_ROOT:/work:Z" \
        -v "$deploy_dir:/work/yocto/build:Z" \
        -e MACHINE="$machine" \
        -e KAS_BUILD_DIR="/work/yocto/build" \
        "$KAS_IMAGE" \
        kas build /work/yocto/kas-iot.yml

    log_info "Artifacts for $machine: $deploy_dir/deploy/"
}

# ── Print summary ───────────────────────────────────────────────────
print_summary() {
    log_section "Build complete"
    echo ""
    echo "  Deploy directory: $BUILD_DIR/"
    echo ""
    for machine in "${MACHINES[@]}"; do
        local d="$BUILD_DIR/$machine/deploy"
        if [ -d "$d/ipk" ]; then
            local count=$(find "$d/ipk" -name '*.ipk' -type f 2>/dev/null | wc -l)
            echo "  $machine  →  $(find "$d/ipk" -name 'iot-*.ipk' -type f 2>/dev/null | wc -l) iot .ipk packages ($count total)"
        fi
    done
    echo ""
    echo "  Install on target:"
    echo "    scp $BUILD_DIR/qemuarm64/deploy/ipk/cortexa57/iot-*.ipk root@<target>:/tmp/"
    echo "    ssh root@<target> opkg install /tmp/iot-ds-server_*.ipk /tmp/iot-lwm2m_*.ipk /tmp/iot-config_*.ipk"
    echo ""
}

# ── Main ────────────────────────────────────────────────────────────
main() {
    detect_runtime
    pull_kas

    local target="${1:-${MACHINE:-qemux86-64}}"

    if [ "$target" = "all" ]; then
        for machine in "${MACHINES[@]}"; do
            build_machine "$machine"
        done
        print_summary
    else
        build_machine "$target"
        print_summary
    fi
}

main "$@"
