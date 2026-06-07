#!/bin/bash
# run.sh — Run the IoT Cloud Server (multi-service via docker compose).
#
# Spawns ds-server, iot-cloudd, and iot-httpd as separate containers
# communicating through a shared Unix socket volume. Prefers docker
# (falls back to podman only if docker is absent).
#
# On start it refreshes the iot-etc config volume from the image so
# schema/config changes always take effect (RESET_CONFIG=0 to keep it).
#
# Usage:
#   ./run.sh                    # start all services on http://localhost:8080
#   ./run.sh stop               # stop all services
#   ./run.sh logs [service]     # tail logs (default: all services)
#   ./run.sh build              # rebuild the image
#   ./run.sh nocache            # rebuild without cache
#   ./run.sh ps                 # list running services
#
#   HTTP_PORT=8443 ./run.sh     # custom HTTP port
#   VPN_SUBNET=10.8.0.0/24 ./run.sh  # custom VPN subnet
#
# Secrets (first run auto-generates):
#   - iot-cloud-ca-key  volume: /run/secrets/iot-ca-key  (CA private key)
#   - iot-cloud-vpn     volume: /etc/iot/vpn              (server certs, PKI)
#
# Persistent state:
#   - iot-etc   volume: /etc/iot  (schemas, config)
#   - iot-lib   volume: /var/lib/iot (persist)
#   - iot-run   volume: /var/run/iot (ds-server socket)

set -euo pipefail

IMAGE="${CLOUD_IMAGE:-docker.io/naushada/iot-cloud:latest}"
HTTP_PORT="${HTTP_PORT:-8080}"
VPN_SUBNET="${VPN_SUBNET:-10.9.0.0/24}"
PROXY_START="${PROXY_START:-5001}"
PROXY_END="${PROXY_END:-6000}"
HTTP_WORKERS="${HTTP_WORKERS:-4}"
# Reset the iot-etc config volume on start so the latest schema/config
# from the image is always loaded (set to 0 to preserve manual edits).
RESET_CONFIG="${RESET_CONFIG:-1}"
# Compose project name → deterministic volume names (cloud_iot-etc, …).
PROJECT="${COMPOSE_PROJECT_NAME:-cloud}"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log_section() { echo -e "\n${GREEN}=== $1 ===${NC}"; }
log_info()    { echo -e "${YELLOW} → $1${NC}"; }

detect_runtime() {
    # Prefer docker; fall back to podman only if docker is unavailable.
    if command -v docker &>/dev/null; then echo "docker"
    elif command -v podman &>/dev/null; then echo "podman"
    else echo "ERROR: docker required (podman also accepted)" >&2; exit 1; fi
}

CR=$(detect_runtime)
COMPOSE="$CR compose"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Export env vars so docker-compose.yml can reference them
export CLOUD_IMAGE="$IMAGE"
export HTTP_PORT HTTP_WORKERS
export VPN_SUBNET PROXY_START PROXY_END
export COMPOSE_PROJECT_NAME="$PROJECT"

case "${1:-start}" in
    build)
        log_section "Building $IMAGE"
        $CR build -t "$IMAGE" -f "$SCRIPT_DIR/Dockerfile" \
            "$SCRIPT_DIR/../../"
        ;;
    nocache|build-nocache)
        log_section "Building $IMAGE (no cache)"
        $CR build --no-cache -t "$IMAGE" -f "$SCRIPT_DIR/Dockerfile" \
            "$SCRIPT_DIR/../../"
        ;;

    start|up)
        # Pull if not present (image inspect works on both docker + podman)
        if ! $CR image inspect "$IMAGE" >/dev/null 2>&1; then
            log_info "Pulling $IMAGE..."
            $CR pull "$IMAGE"
        fi

        # Stop old single-container if running (migration from v1)
        $CR stop iot-cloud 2>/dev/null && log_info "Stopped legacy iot-cloud container" || true
        $CR rm iot-cloud 2>/dev/null || true

        # Always reload schemas/config from the image. The iot-etc volume
        # (/etc/iot) holds image-provided static config — Lua schemas +
        # apps/config. Persisting it across image rebuilds causes stale
        # schemas: new data-store keys get rejected and silently read null
        # (e.g. the L22 services.*.cpu.permille telemetry, where only
        # ds-server — which writes in-process, bypassing schema — showed
        # data). Dropping it forces a repopulate from the current image.
        # Persisted data (/var/lib/iot) and VPN PKI (/etc/iot/vpn,
        # /run/secrets) live in separate volumes and are untouched.
        if [ "$RESET_CONFIG" = "1" ]; then
            log_info "Refreshing config volume ${PROJECT}_iot-etc (set RESET_CONFIG=0 to keep)"
            $COMPOSE -f "$SCRIPT_DIR/docker-compose.yml" down --remove-orphans 2>/dev/null || true
            $CR volume rm "${PROJECT}_iot-etc" 2>/dev/null || true
        fi

        log_section "Starting IoT Cloud"
        $COMPOSE -f "$SCRIPT_DIR/docker-compose.yml" up -d

        sleep 3
        echo ""
        echo -e "  ${GREEN}Cloud UI:${NC}    http://localhost:$HTTP_PORT"
        echo -e "  ${GREEN}Login:${NC}       admin / admin"
        echo -e "  ${GREEN}LwM2M BS:${NC}   coaps://localhost:5684"
        echo -e "  ${GREEN}LwM2M DM:${NC}   coaps://localhost:5683"
        echo -e "  ${GREEN}OpenVPN:${NC}    udp://localhost:1194"
        echo ""
        echo "  ./run.sh logs     — tail logs (all services)"
        echo "  ./run.sh ps       — list services"
        echo "  ./run.sh stop     — stop all"
        ;;

    stop|down)
        log_section "Stopping IoT Cloud"
        $COMPOSE -f "$SCRIPT_DIR/docker-compose.yml" down
        ;;

    logs)
        echo "=== Logs ==="
        $CR exec iot-ds-server ds-cli get log.text log.cloudd.text \
            log.lwm2m.text log.lwm2m.bs.text log.lwm2m.dm.text 2>&1
        echo ""
        echo "=== Services ==="
        $CR exec iot-ds-server ds-cli get services.ds.state \
            services.cloud.iot.cloudd.state \
            services.cloud.iot.httpd.state \
            services.cloud.openvpn.server.state \
            services.cloud.lwm2m.bs.state \
            services.cloud.lwm2m.dm.state 2>&1
        ;;

    ps|status)
        $COMPOSE -f "$SCRIPT_DIR/docker-compose.yml" ps
        ;;

    restart)
        $COMPOSE -f "$SCRIPT_DIR/docker-compose.yml" restart "${@}"
        ;;

    *)
        echo "Usage: $0 {start|stop|logs|ps|build|restart}" >&2
        exit 1
        ;;
esac
