#!/bin/bash
# build.sh — Containerised multi-arch Yocto build for the iot LwM2M stack
#
# Builds inside a podman/docker container, then copies artifacts to the
# host. No Yocto install needed on the host — just podman or docker.
#
# Usage:
#   ./build.sh                        # qemux86-64 (default)
#   MACHINE=qemuarm64 ./build.sh      # ARM64 / aarch64
#   MACHINE=qemuarm ./build.sh        # ARMv7 / armhf
#   ./build.sh all                    # All three architectures
#
# Output per machine:
#   yocto/build/<machine>/ipk/        *.ipk packages
#   yocto/build/<machine>/images/     rootfs / kernel images
#
# Requirements:
#   - podman or docker
#   - ~30 GB free disk space (Yocto downloads + build)
#   - Internet access (first build fetches ~8 GB of sources)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$SCRIPT_DIR/build"
IMAGE_NAME="${IMAGE_NAME:-iot-yocto-builder:latest}"

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
        packagegroup-iot

    # Copy artifacts from the container to the host
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
        local d="$OUT_DIR/$machine/ipk"
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
    echo "    scp $OUT_DIR/qemuarm64/ipk/*/iot-*.ipk root@<target>:/tmp/"
    echo "    ssh root@<target> opkg install /tmp/iot-ds-server_*.ipk /tmp/iot-lwm2m_*.ipk /tmp/iot-config_*.ipk"
    echo ""
}

# ── Main ──────────────────────────────────────────────────────────────
main() {
    CR=$(detect_runtime)
    log_info "Container runtime: $CR"

    build_image

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
