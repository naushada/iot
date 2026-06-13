#!/bin/bash
# run.sh — Run the IoT Device stack (dev / e2e, no RPi3B board).
#
# Spawns ds-server, a local lwm2m server, the lwm2m device client, and
# iot-httpd as separate containers communicating through a shared Unix
# socket volume — the same daemon split that runs on a real board via
# systemd. Prefers docker (falls back to podman only if docker is absent).
#
# This image is built locally (not published), so the default is to build
# rather than pull. Build once, then start:
#   ./run.sh build              # build naushada/iot-device:latest
#   ./run.sh                    # start all services
#
# Usage:
#   ./run.sh                    # start all services; UI on http://localhost:8081
#   ./run.sh stop               # stop all services
#   ./run.sh logs [service]     # tail logs (default: all services)
#   ./run.sh build              # build the image
#   ./run.sh nocache            # build without cache
#   ./run.sh ps                 # list running services
#   ./run.sh ds get log.text    # run ds-cli inside the ds-server container
#
#   HTTP_PORT=9090 ./run.sh                          # custom UI host port
#
# The bootstrap server (iot.bs.uri) and the VPN server endpoint
# (vpn.remote.*) are NOT set via env — they live in the data store
# (persisted in the dev-lib volume). Provision iot.bs.uri once by
# commissioning in device-ui (./run.sh commission on, then set serial +
# bootstrap URI + generate BS PSK); the VPN endpoint is pushed by the cloud
# over LwM2M Object 2048 after the device registers. ds is the single
# source of truth.
#
# Persistent state (named volumes):
#   - dev-etc   volume: /etc/iot      (schemas, env files, LwM2M config)
#   - dev-lib   volume: /var/lib/iot  (persisted data store)
#   - dev-run   volume: /run/iot      (ds-server socket)

set -euo pipefail

IMAGE="${DEVICE_IMAGE:-docker.io/naushada/iot-device:latest}"
HTTP_PORT="${HTTP_PORT:-8081}"
# Reset the dev-etc config volume on start so the latest schema/config
# from the image is always loaded (set to 0 to preserve manual edits).
RESET_CONFIG="${RESET_CONFIG:-1}"
# Compose project name → deterministic volume names (device_dev-etc, …).
PROJECT="${COMPOSE_PROJECT_NAME:-device}"
# Published to Docker Hub by CI (device-image.yml) — pull by default.
# PULL=0 uses a local `./run.sh build` instead.
PULL="${PULL:-1}"

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log_section() { echo -e "\n${GREEN}=== $1 ===${NC}"; }
log_info()    { echo -e "${YELLOW} → $1${NC}"; }

# Remove dangling (<none>) images left behind by a rebuild. Works on both
# docker and podman (same CLI). `|| true` keeps set -e happy when there is
# nothing to prune.
prune_dangling() {
    log_info "Pruning dangling images…"
    $CR image prune -f >/dev/null 2>&1 || true
}

detect_runtime() {
    if command -v docker &>/dev/null; then echo "docker"
    elif command -v podman &>/dev/null; then echo "podman"
    else echo "ERROR: docker required (podman also accepted)" >&2; exit 1; fi
}

CR=$(detect_runtime)
COMPOSE="$CR compose"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Export env so docker-compose.yml can reference them.
export DEVICE_IMAGE="$IMAGE"
export HTTP_PORT
export COMPOSE_PROJECT_NAME="$PROJECT"

case "${1:-start}" in
    build)
        log_section "Building $IMAGE"
        $CR build -t "$IMAGE" -f "$SCRIPT_DIR/Dockerfile" "$SCRIPT_DIR/../../"
        prune_dangling
        log_info "Built — start it with: ./run.sh"
        ;;
    nocache|build-nocache)
        log_section "Building $IMAGE (no cache)"
        $CR build --no-cache -t "$IMAGE" -f "$SCRIPT_DIR/Dockerfile" "$SCRIPT_DIR/../../"
        prune_dangling
        log_info "Built — start it with: ./run.sh"
        ;;

    start|up)
        if [ "$PULL" = "1" ] || ! $CR image inspect "$IMAGE" >/dev/null 2>&1; then
            log_info "Pulling $IMAGE..."
            $CR pull "$IMAGE" || log_info "pull failed — using local image if present"
        fi
        if ! $CR image inspect "$IMAGE" >/dev/null 2>&1; then
            echo "ERROR: image '$IMAGE' not found — pull failed and no local image. Build it: ./run.sh build" >&2
            exit 1
        fi

        # Reload schemas/config from the image. dev-etc (/etc/iot) holds
        # image-provided static config (Lua schemas + LwM2M object config);
        # persisting it across rebuilds causes stale schemas. Persisted
        # data (dev-lib) and the socket (dev-run) are untouched.
        if [ "$RESET_CONFIG" = "1" ]; then
            log_info "Refreshing config volume ${PROJECT}_dev-etc (set RESET_CONFIG=0 to keep)"
            $COMPOSE -f "$SCRIPT_DIR/docker-compose.yml" down --remove-orphans 2>/dev/null || true
            $CR volume rm "${PROJECT}_dev-etc" 2>/dev/null || true
        fi

        log_section "Starting IoT Device stack"
        $COMPOSE -f "$SCRIPT_DIR/docker-compose.yml" up -d

        sleep 3
        echo ""
        echo -e "  ${GREEN}Device UI:${NC}    http://localhost:$HTTP_PORT"
        echo -e "  ${GREEN}Bootstrap:${NC}    iot.bs.uri (data-store) — set via device-ui commissioning"
        echo ""
        echo "  ./run.sh logs [service]  — tail logs"
        echo "  ./run.sh ps              — list services"
        echo "  ./run.sh ds get log.lwm2m.text  — read a ds key"
        echo "  ./run.sh stop            — stop all"
        ;;

    stop|down)
        log_section "Stopping IoT Device stack"
        $COMPOSE -f "$SCRIPT_DIR/docker-compose.yml" down
        ;;

    logs)
        shift || true
        $COMPOSE -f "$SCRIPT_DIR/docker-compose.yml" logs -f ${@+"${@}"}
        ;;

    ps|status)
        $COMPOSE -f "$SCRIPT_DIR/docker-compose.yml" ps
        ;;

    restart)
        shift || true
        $COMPOSE -f "$SCRIPT_DIR/docker-compose.yml" restart "${@}"
        ;;

    ds)
        # Forward to ds-cli inside the ds-server container (runs as root):
        #   ./run.sh ds get log.lwm2m.text
        #   ./run.sh ds set iot.endpoint '"urn:dev:test-1"'
        # NOTE: root cannot write gid:engineer keys (serial/PSK/dev.mode) —
        # use `./run.sh commission` for those.
        shift || true
        $CR exec iot-dev-ds ds-cli --socket=/run/iot/data_store.sock "${@}"
        ;;

    commission)
        # Enter/leave commissioning mode by setting iot.dev.mode. This key is
        # write_acl=gid:engineer, so we run ds-cli inside the lwm2m-client
        # container, which runs as the `engineer` account — root (the `ds`
        # command / ds-cli on the host) is denied. While on, ds-server
        # bypasses the PSK ACLs so device-ui can write the serial + BS PSK.
        #   ./run.sh commission on    # enable, then set serial/PSK in device-ui
        #   ./run.sh commission off   # lock down after provisioning
        case "${2:-on}" in
            on|true|1)  val=true ;;
            off|false|0) val=false ;;
            *) echo "Usage: $0 commission [on|off]" >&2; exit 1 ;;
        esac
        # Use a one-off container running as the engineer account (not exec in
        # the client, which crash-loops while unprovisioned). engineer's
        # gid:engineer satisfies the write_acl on iot.dev.mode; --group-add iot
        # lets it reach the ds socket (group iot).
        log_info "Setting iot.dev.mode=$val (one-off engineer container)"
        $CR run --rm --user engineer:engineer --group-add iot \
            -v "${PROJECT}_dev-run":/run/iot "$IMAGE" \
            ds-cli --socket=/run/iot/data_store.sock set iot.dev.mode "$val"
        ;;

    *)
        echo "Usage: $0 {start|stop|logs|ps|build|nocache|restart|ds|commission}" >&2
        exit 1
        ;;
esac
