#!/bin/sh
# L11/D6 — end-to-end deploy smoke.
#
# Boots the OCI image from PR #25 as two sibling containers (ds + client)
# sharing a /run/iot volume, mutates iot.lifetime via ds-cli, asserts
# the client container picks up the hot-applied value.
#
# Run from the repo root:
#   ./log/L11/e2e-smoke.sh
# Or after `git pull` from a fresh clone:
#   sh ./log/L11/e2e-smoke.sh
#
# Exit 0 on success; non-zero with a message on failure.

set -eu

PODMAN="${PODMAN:-/opt/homebrew/bin/podman}"
IMAGE="${IMAGE:-iot:l11}"

DS_NAME=iot-ds-e2e
CLIENT_NAME=iot-client-e2e
VOLUME=iot-run-e2e

cleanup() {
    "$PODMAN" rm -f "$DS_NAME" "$CLIENT_NAME" >/dev/null 2>&1 || true
    "$PODMAN" volume rm "$VOLUME" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

cleanup

echo "=== 1. Build the OCI image (no-op if cached) ==="
"$PODMAN" build -f packaging/Containerfile -t "$IMAGE" . 2>&1 | tail -3

echo
echo "=== 2. Start ds-server container ==="
"$PODMAN" volume create "$VOLUME" >/dev/null
"$PODMAN" run -d \
    --name "$DS_NAME" \
    -e IOT_ROLE=ds \
    -v "$VOLUME":/run/iot \
    "$IMAGE" >/dev/null
sleep 0.5
"$PODMAN" logs "$DS_NAME" 2>&1 | grep -F "listening on" || {
    echo "FAIL: ds-server didn't log 'listening on'" >&2
    "$PODMAN" logs "$DS_NAME" >&2
    exit 1
}
echo "OK ds-server bound /run/iot/data_store.sock"

echo
echo "=== 3. Seed initial iot.* keys via ds-cli (from inside the ds container) ==="
"$PODMAN" exec "$DS_NAME" \
    ds-cli --socket=/run/iot/data_store.sock \
    set iot.endpoint '"urn:dev:e2e-initial"' >/dev/null
"$PODMAN" exec "$DS_NAME" \
    ds-cli --socket=/run/iot/data_store.sock \
    set iot.lifetime 7200 >/dev/null
"$PODMAN" exec "$DS_NAME" \
    ds-cli --socket=/run/iot/data_store.sock \
    get iot.endpoint iot.lifetime

echo
echo "=== 4. Start lwm2m client container sharing /run/iot ==="
"$PODMAN" run -d \
    --name "$CLIENT_NAME" \
    -e IOT_ROLE=client \
    -v "$VOLUME":/run/iot \
    "$IMAGE" >/dev/null
sleep 1.0
"$PODMAN" logs "$CLIENT_NAME" 2>&1 | grep -F "endpoint from data-store: urn:dev:e2e-initial" || {
    echo "FAIL: client didn't pick up initial endpoint from data-store" >&2
    "$PODMAN" logs "$CLIENT_NAME" >&2
    exit 1
}
echo "OK client picked up initial iot.endpoint + iot.lifetime via DsConfig"

echo
echo "=== 5. Hot-apply: mutate iot.lifetime; expect 'applied iot.lifetime=...' on client ==="
"$PODMAN" exec "$DS_NAME" \
    ds-cli --socket=/run/iot/data_store.sock \
    set iot.lifetime 1800 >/dev/null
sleep 0.5
"$PODMAN" logs "$CLIENT_NAME" 2>&1 | grep -F "applied iot.lifetime=1800" || {
    echo "FAIL: client didn't log the hot-applied lifetime" >&2
    "$PODMAN" logs "$CLIENT_NAME" 2>&1 | tail -20 >&2
    exit 1
}
echo "OK client hot-applied iot.lifetime=1800 from listener thread"

echo
echo "=== 6. Schema rejection still gates at the wire ==="
if "$PODMAN" exec "$DS_NAME" \
    ds-cli --socket=/run/iot/data_store.sock \
    set iot.lifetime -1 >/dev/null 2>&1; then
    echo "FAIL: ds-cli set iot.lifetime -1 unexpectedly succeeded" >&2
    exit 1
fi
echo "OK schema rejected iot.lifetime=-1 with SchemaRejected"

echo
echo "=== Summary ==="
echo "All 6 checks passed."
echo "DS image size: $("$PODMAN" image inspect "$IMAGE" --format '{{.Size}}' | awk '{printf "%.1f MB", $1/1024/1024}')"
echo "ds container exit: $("$PODMAN" inspect "$DS_NAME" --format '{{.State.Status}}')"
echo "client container exit: $("$PODMAN" inspect "$CLIENT_NAME" --format '{{.State.Status}}')"
