#!/usr/bin/env bash
# Crash-style restart smoke for ds-server's Lua persistor (D4).
#
# Spins ds-server with a host-mounted persistence directory, sets a
# few keys, kills the server, restarts with the same store path,
# verifies the keys survived. Saves the transcript (including the
# on-disk Lua chunk) to log/L10/ds-persist-smoke.txt.
#
# Build prerequisite (same as run-ds-smoke.sh):
#   cd modules/data-store && mkdir -p build && cd build && \
#     cmake .. -DBUILD_DATA_STORE_TESTS=ON && make -j2

set -euo pipefail

PODMAN=/opt/homebrew/bin/podman
NAME=ds-persist
SOCK=/tmp/ds-persist.sock
STORE_IN_CTR=/var/lib/iot/data.lua
HOST_PERSIST=$(mktemp -d -t ds-persist-XXXXXX)
BUILD_DIR="$PWD/modules/data-store/build"
TRANSCRIPT=log/L10/ds-persist-smoke.txt

if [ ! -x "$BUILD_DIR/ds-server" ] || [ ! -x "$BUILD_DIR/ds-cli" ]; then
    echo "[smoke] ds-server / ds-cli not built." >&2
    exit 1
fi

: > "$TRANSCRIPT"
log() { printf '%s\n' "$*" | tee -a "$TRANSCRIPT"; }

cleanup() {
    $PODMAN kill -s SIGKILL $NAME >/dev/null 2>&1 || true
    $PODMAN rm   -f         $NAME >/dev/null 2>&1 || true
    rm -rf "$HOST_PERSIST"
}
trap cleanup EXIT

start_server() {
    $PODMAN run -d --rm --name $NAME \
        -v "$BUILD_DIR/ds-server":/opt/ds/ds-server \
        -v "$BUILD_DIR/ds-cli":/opt/ds/ds-cli \
        -v "$HOST_PERSIST":/var/lib/iot \
        --entrypoint /bin/bash \
        naushada/iot:latest \
        -c "mkdir -p /tmp && chmod 1777 /tmp && exec /opt/ds/ds-server ds-socket=$SOCK ds-store=$STORE_IN_CTR" \
        >/dev/null
    sleep 1
}

in_cli() {
    $PODMAN exec $NAME /opt/ds/ds-cli --socket=$SOCK "$@"
}

log "[persist] host persist dir = $HOST_PERSIST"
log "[persist] starting ds-server (round 1)"
start_server

log "----- ds-cli set foo bar -----"
in_cli set foo bar | tee -a "$TRANSCRIPT"

log "----- ds-cli set counter 42 -----"
in_cli set counter 42 | tee -a "$TRANSCRIPT"

log "----- on-disk Lua chunk (host-side) -----"
cat "$HOST_PERSIST/data.lua" | tee -a "$TRANSCRIPT"

log "[persist] killing ds-server (simulated crash)"
$PODMAN kill $NAME >/dev/null 2>&1 || true
sleep 1

log "[persist] starting ds-server (round 2 — should reload state)"
start_server
sleep 1
log "----- ds-server stdout (round 2) -----"
{ $PODMAN logs $NAME 2>&1 | grep -E 'loaded|store' || true; } | tee -a "$TRANSCRIPT"

log "----- ds-cli get foo counter missing -----"
in_cli get foo counter missing | tee -a "$TRANSCRIPT"

log "[persist] done; transcript → $TRANSCRIPT"
