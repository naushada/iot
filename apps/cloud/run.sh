#!/bin/bash
# run.sh — Run the IoT Cloud Server container.
#
# Pulls naushada/iot-cloud:latest if not present locally, starts
# ds-server + iot-httpd + iot-cloudd.  Uses podman or docker.
#
# Usage:
#   ./run.sh                  # start on http://localhost:8080
#   ./run.sh stop             # stop the container
#   ./run.sh logs             # tail logs
#   HTTP_PORT=8443 ./run.sh   # custom HTTP port
#   VPN_PORT=1194 ./run.sh    # custom OpenVPN port
#
# Secrets (first run auto-generates):
#   - iot-cloud-ca-key  volume: /run/secrets/iot-ca-key  (CA private key)
#   - iot-cloud-vpn     volume: /etc/iot/vpn              (server certs, PKI)
#
# Persistent state:
#   - iot-cloud-etc   volume: /etc/iot  (schemas, config)
#   - iot-cloud-run   volume: /var/run/iot (ds-server socket)

set -euo pipefail

IMAGE="${CLOUD_IMAGE:-docker.io/naushada/iot-cloud:latest}"
NAME="${CLOUD_NAME:-iot-cloud}"
HTTP_PORT="${HTTP_PORT:-8080}"
VPN_PORT="${VPN_PORT:-1194}"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log_section() { echo -e "\n${GREEN}=== $1 ===${NC}"; }
log_info()    { echo -e "${YELLOW} → $1${NC}"; }

detect_runtime() {
    if command -v podman &>/dev/null; then echo "podman"
    elif command -v docker &>/dev/null; then echo "docker"
    else echo "ERROR: podman or docker required" >&2; exit 1; fi
}

CR=$(detect_runtime)

# ── First-run: generate CA key if it doesn't exist ─────────────
ensure_ca_key() {
    local vol="iot-cloud-ca-key"
    if ! $CR volume exists "$vol" 2>/dev/null; then
        log_info "Generating CA private key (first run)..."
        $CR volume create "$vol" >/dev/null
        $CR run --rm -v "$vol:/run/secrets/iot-ca-key" \
            --entrypoint openssl "$IMAGE" \
            genrsa -out /run/secrets/iot-ca-key/ca.key 4096 2>/dev/null
        log_info "CA key created in volume: $vol"
    fi
}

case "${1:-start}" in
    stop)
        $CR stop "$NAME" 2>/dev/null && log_info "Stopped $NAME" || echo "Not running"
        ;;
    logs)
        $CR logs -f "$NAME"
        ;;
    start|run)
        # Pull if not present
        if ! $CR image exists "$IMAGE" 2>/dev/null; then
            log_info "Pulling $IMAGE..."
            $CR pull "$IMAGE"
        fi

        # Stop if already running
        $CR stop "$NAME" 2>/dev/null || true
        $CR rm "$NAME" 2>/dev/null || true

        # Ensure volumes exist
        for vol in iot-cloud-etc iot-cloud-run iot-cloud-vpn; do
            $CR volume create "$vol" 2>/dev/null || true
        done
        ensure_ca_key

        log_section "Starting IoT Cloud"
        $CR run -d --name "$NAME" \
            -p "$HTTP_PORT:8080" \
            -p 5683:5683 -p 5684:5684 \
            -p "$VPN_PORT:1194/udp" \
            --cap-add NET_ADMIN \
            -v iot-cloud-etc:/etc/iot \
            -v iot-cloud-run:/var/run/iot \
            -v iot-cloud-vpn:/etc/iot/vpn \
            -v iot-cloud-ca-key:/run/secrets/iot-ca-key:ro \
            --restart unless-stopped \
            "$IMAGE"

        sleep 3
        echo ""
        echo -e "  ${GREEN}Cloud UI:${NC}    http://localhost:$HTTP_PORT"
        echo -e "  ${GREEN}Login:${NC}       admin / admin"
        echo -e "  ${GREEN}LwM2M BS:${NC}   coaps://localhost:5684"
        echo -e "  ${GREEN}LwM2M DM:${NC}   coaps://localhost:5683"
        echo -e "  ${GREEN}OpenVPN:${NC}    udp://localhost:$VPN_PORT"
        echo ""
        echo "  ./run.sh logs   — tail logs"
        echo "  ./run.sh stop   — stop container"
        ;;
    *)
        echo "Usage: $0 {start|stop|logs}" >&2; exit 1
        ;;
esac
