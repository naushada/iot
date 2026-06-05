#!/bin/bash
# serve.sh — Run the iot-ui Angular dev server inside podman.
#
# No local Node.js install needed.  The source tree is bind-mounted so
# live-reload picks up edits instantly.
#
# Usage:
#   ./serve.sh              # dev server on http://localhost:4200
#   ./serve.sh build        # production build → dist/iot-ui/
#
# The first run builds the iot-ui-dev image (~5 min); subsequent runs
# are instant.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IMAGE="iot-ui-dev:latest"

detect_runtime() {
    if command -v podman &>/dev/null; then echo "podman"
    elif command -v docker &>/dev/null; then echo "docker"
    else echo "ERROR: podman or docker required" >&2; exit 1; fi
}

CR=$(detect_runtime)

case "${1:-serve}" in
    build)
        echo "=== Building iot-ui (production) ==="
        $CR run --rm \
            -v "$SCRIPT_DIR:/app:Z" \
            "$IMAGE" \
            ng build --configuration production
        echo "Output: $SCRIPT_DIR/dist/iot-ui/"
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
        echo "=== iot-ui dev server → http://localhost:4200 ==="
        $CR run --rm -it \
            -p 4200:4200 \
            -v "$SCRIPT_DIR:/app:Z" \
            "$IMAGE"
        ;;
esac
