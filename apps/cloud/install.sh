#!/bin/bash
# install.sh — Build the IoT Cloud Server image and run it.
#
# Containerised: builds all C++ modules + cloud UI, packages into a
# single OCI image.  Works with podman or docker.
#
# Usage:
#   ./install.sh              # build image
#   ./install.sh run          # build + run (http://localhost:8080)
#   ./install.sh shell        # open a shell inside the container
#
# Requirements:
#   - podman or docker
#   - ~4 GB free disk (build deps + ACE compile)
#
# Environment:
#   CLOUD_IMAGE   image name (default: iot-cloud:latest)
#   HTTP_PORT     host port for the web UI (default: 8080)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
IMAGE="${CLOUD_IMAGE:-iot-cloud:latest}"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log_section() { echo -e "\n${GREEN}=== $1 ===${NC}"; }
log_info()    { echo -e "${YELLOW} → $1${NC}"; }
log_error()   { echo -e "${RED}ERROR: $1${NC}" >&2; }

detect_runtime() {
    if command -v podman &>/dev/null; then echo "podman"
    elif command -v docker &>/dev/null; then echo "docker"
    else log_error "podman or docker required"; exit 1; fi
}

CR=$(detect_runtime)

case "${1:-build}" in
    run)
        if ! $CR image exists "$IMAGE" 2>/dev/null; then
            log_info "Image not found — building first..."
            "$0" build
        fi
        HTTP_PORT="${HTTP_PORT:-8080}"
        log_section "Starting IoT Cloud on http://localhost:$HTTP_PORT"
        $CR run --rm -it \
            -p "$HTTP_PORT:8080" \
            -p 5683:5683 -p 5684:5684 \
            --cap-add NET_ADMIN \
            "$IMAGE"
        ;;
    shell)
        $CR run --rm -it --entrypoint bash "$IMAGE"
        ;;
    build)
        log_section "Building $IMAGE"
        cd "$REPO_ROOT"
        $CR build -t "$IMAGE" -f "$SCRIPT_DIR/Containerfile" .
        log_info "Image ready: $IMAGE"
        echo ""
        echo "  Run:   ./install.sh run"
        echo "  Shell: ./install.sh shell"
        ;;
    *)
        echo "Usage: $0 {build|run|shell}" >&2; exit 1
        ;;
esac
