#!/bin/bash
# run.sh — Run the IoT Cloud Server container.
#
# Pulls naushada/iot-cloud:latest if not present locally, then starts
# ds-server + iot-httpd with the cloud UI.  Uses podman or docker.
#
# Usage:
#   ./run.sh                  # start on http://localhost:8080
#   ./run.sh stop             # stop the container
#   ./run.sh logs             # tail logs
#   HTTP_PORT=8443 ./run.sh   # custom port
#
# Persistent state:
#   - iot-cloud-etc   volume: /etc/iot  (schemas, config)
#   - iot-cloud-run    volume: /var/run/iot (ds-server socket)

set -euo pipefail

IMAGE="${CLOUD_IMAGE:-docker.io/naushada/iot-cloud:latest}"
NAME="${CLOUD_NAME:-iot-cloud}"
HTTP_PORT="${HTTP_PORT:-8080}"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log_info() { echo -e "${YELLOW} → $1${NC}"; }

detect_runtime() {
    if command -v podman &>/dev/null; then echo "podman"
    elif command -v docker &>/dev/null; then echo "docker"
    else echo "ERROR: podman or docker required" >&2; exit 1; fi
}

CR=$(detect_runtime)

case "${1:-start}" in
    stop)
        $CR stop "$NAME" 2>/dev/null && log_info "Stopped $NAME" || echo "Not running"
        ;;
    logs)
        $CR logs -f "$NAME"
        ;;
    start)
        # Pull if not present
        if ! $CR image exists "$IMAGE" 2>/dev/null; then
            log_info "Pulling $IMAGE..."
            $CR pull "$IMAGE"
        fi

        # Stop if already running
        $CR stop "$NAME" 2>/dev/null || true
        $CR rm "$NAME" 2>/dev/null || true

        # Create volumes for persistent state
        $CR volume create iot-cloud-etc 2>/dev/null || true
        $CR volume create iot-cloud-run 2>/dev/null || true

        log_info "Starting $NAME on http://localhost:$HTTP_PORT"
        $CR run -d --name "$NAME" \
            -p "$HTTP_PORT:8080" \
            -p 5683:5683 -p 5684:5684 \
            --cap-add NET_ADMIN \
            -v iot-cloud-etc:/etc/iot \
            -v iot-cloud-run:/var/run/iot \
            --restart unless-stopped \
            "$IMAGE"

        sleep 2
        log_info "Cloud UI: http://localhost:$HTTP_PORT"
        log_info "Login: admin / admin"
        ;;
    *)
        echo "Usage: $0 {start|stop|logs}" >&2; exit 1
        ;;
esac
