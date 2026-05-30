#!/usr/bin/env bash
# End-to-end smoke for ds-server + ds-cli.
#
# Spins one ds-server inside a podman container, drives it from the
# host via ds-cli (also baked into the same container; we invoke it
# through `podman exec` so they share the same UDS path). Exercises:
#
#   ds-cli welcome           → expect '{"ok":true,"hello":"data-store-server",...}'
#   ds-cli set foo bar       → 'ok'
#   ds-cli get foo missing   → 'foo=bar' + 'missing=(null)'
#   ds-cli watch foo (bg)    → reads one event
#   ds-cli set foo baz       → triggers the event
#   ds-cli unwatch foo       → 'ok'
#
# Full transcript saved to log/L10/ds-smoke.txt.
#
# Build the targets first:
#   cd modules/data-store && mkdir -p build && cd build && \
#     cmake .. -DBUILD_DATA_STORE_TESTS=ON && make -j2
#
# Usage:  bash log/L10/run-ds-smoke.sh

set -euo pipefail

PODMAN=/opt/homebrew/bin/podman
NAME=ds-smoke
SOCK=/tmp/ds-smoke.sock
TRANSCRIPT=log/L10/ds-smoke.txt
BUILD_DIR="$PWD/modules/data-store/build"

if [ ! -x "$BUILD_DIR/ds-server" ] || [ ! -x "$BUILD_DIR/ds-cli" ]; then
    echo "[smoke] ds-server / ds-cli not built. Run the cmake+make snippet above." >&2
    exit 1
fi

cleanup() {
    $PODMAN kill -s SIGKILL $NAME >/dev/null 2>&1 || true
    $PODMAN rm   -f         $NAME >/dev/null 2>&1 || true
}
trap cleanup EXIT

: > "$TRANSCRIPT"

log() { printf '%s\n' "$*" | tee -a "$TRANSCRIPT"; }

# ds-cli helper: runs the binary inside the same container as the
# server so they share /tmp.
in_cli() {
    $PODMAN exec $NAME /opt/ds/ds-cli --socket=$SOCK "$@"
}

log "[smoke] starting ds-server in podman ($NAME)"
$PODMAN run -d --rm --name $NAME \
    -v "$BUILD_DIR/ds-server":/opt/ds/ds-server \
    -v "$BUILD_DIR/ds-cli":/opt/ds/ds-cli \
    --entrypoint /bin/bash \
    naushada/iot:latest \
    -c "mkdir -p /tmp && chmod 1777 /tmp && exec /opt/ds/ds-server ds-socket=$SOCK ds-store=/tmp/ds-smoke.lua" >/dev/null
sleep 1

log "----- ds-cli welcome -----"
in_cli welcome      | tee -a "$TRANSCRIPT"

log "----- ds-cli set foo bar -----"
in_cli set foo bar  | tee -a "$TRANSCRIPT"

log "----- ds-cli get foo missing -----"
in_cli get foo missing | tee -a "$TRANSCRIPT"

log "----- ds-cli watch foo (bg) -----"
WATCH_OUT=$(mktemp)
in_cli watch --count=1 foo >"$WATCH_OUT" 2>&1 &
WPID=$!

sleep 0.5
log "----- ds-cli set foo baz (triggers event) -----"
in_cli set foo baz  | tee -a "$TRANSCRIPT"

# Give the watcher time to receive + write its event.
wait $WPID 2>/dev/null || true
log "----- watch output -----"
cat "$WATCH_OUT" | tee -a "$TRANSCRIPT"
rm -f "$WATCH_OUT"

log "----- ds-cli unwatch foo -----"
in_cli unwatch foo  | tee -a "$TRANSCRIPT"

log "[smoke] transcript → $TRANSCRIPT"
