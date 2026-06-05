#!/bin/bash
# serve.sh — Run the cloud-ui Angular dev server inside podman.
#
# No local Node.js install needed.  The source tree is bind-mounted so
# live-reload picks up edits instantly.
#
# Usage:
#   ./serve.sh              # dev server on http://localhost:4300
#   ./serve.sh build        # production build → dist/cloud-ui/
#
# The first run builds the cloud-ui-dev image (~5 min); subsequent runs
# are instant.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IMAGE="cloud-ui-dev:latest"

detect_runtime() {
    if command -v podman &>/dev/null; then echo "podman"
    elif command -v docker &>/dev/null; then echo "docker"
    else echo "ERROR: podman or docker required" >&2; exit 1; fi
}

CR=$(detect_runtime)

case "${1:-serve}" in
    build)
        echo "=== Building cloud-ui (production) ==="
        $CR run --rm \
            -v "$SCRIPT_DIR:/app:Z" \
            "$IMAGE" \
            ng build --configuration production
        echo "Output: $SCRIPT_DIR/dist/cloud-ui/"
        ;;
    install)
        echo "=== Installing dependencies ==="
        $CR run --rm \
            -v "$SCRIPT_DIR:/app:Z" \
            "$IMAGE" \
            npm install
        ;;
    serve|*)
        # Build the dev image on first run
        if ! $CR image exists "$IMAGE" 2>/dev/null; then
            echo "=== Building $IMAGE (first run) ==="
            $CR build -t "$IMAGE" -f "$SCRIPT_DIR/Containerfile" "$SCRIPT_DIR"
        fi
        echo "=== cloud-ui dev server → http://localhost:4300 ==="
        $CR run --rm -it \
            -p 4300:4300 \
            -v "$SCRIPT_DIR:/app:Z" \
            "$IMAGE"
        ;;
esac
