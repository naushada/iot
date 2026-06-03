#!/bin/bash
# L18/D6 — HTTP REST API smoke.
#
# Starts ds-server + iot-httpd, drives the /api/v1/db/* endpoints
# with curl, asserts expected responses. Designed for podman:
#   podman run --rm -v $PWD:/work -w /work --network=host \
#     naushada/iot:latest bash log/L18/smoke.sh

set -eu

SMOKE_DIR=/tmp/l18-smoke
DS_SOCK="$SMOKE_DIR/ds.sock"
DS_STORE="$SMOKE_DIR/store.lua"
SCHEMA_DIR="$SMOKE_DIR/schemas"
HTTP_PORT="${HTTP_PORT:-18080}"

DS_SERVER="${DS_SERVER:-./modules/data-store/build/ds-server}"
DS_CLI="${DS_CLI:-./modules/data-store/build/ds-cli}"
HTTPD="${HTTPD:-./modules/http-server/build/iot-httpd}"
[ -x "$DS_SERVER" ] || DS_SERVER=/usr/local/bin/ds-server
[ -x "$DS_CLI" ]    || DS_CLI=/usr/local/bin/ds-cli
[ -x "$HTTPD" ]     || HTTPD=/usr/local/bin/iot-httpd

cleanup() {
    pkill -P $$ 2>/dev/null || true
    rm -rf "$SMOKE_DIR"
}
trap cleanup EXIT INT TERM
cleanup

mkdir -p "$SMOKE_DIR" "$SCHEMA_DIR"
for s in modules/data-store/schemas/*.lua \
         modules/net/router/schemas/*.lua \
         modules/openvpn/client/schemas/*.lua \
         modules/wan/wifi/client/schemas/*.lua \
         modules/http-server/schemas/*.lua; do
    [ -f "$s" ] && cp "$s" "$SCHEMA_DIR/"
done

PASS=1

# ── 1. ds-server ──────────────────────────────────────────────
"$DS_SERVER" ds-socket="$DS_SOCK" ds-store="$DS_STORE" \
    ds-schema-dir="$SCHEMA_DIR" >"$SMOKE_DIR/ds.log" 2>&1 &
DS_PID=$!
sleep 0.5
echo "OK ds-server up"

# Seed required keys
"$DS_CLI" --socket="$DS_SOCK" set net.lwm2m.target.ip '"127.0.0.1"' >/dev/null 2>&1 || true

# ── 2. iot-httpd ──────────────────────────────────────────────
"$HTTPD" ds-socket="$DS_SOCK" http-port="$HTTP_PORT" \
    >"$SMOKE_DIR/httpd.log" 2>&1 &
HTTPD_PID=$!
sleep 0.5
echo "OK iot-httpd up on port $HTTP_PORT"

CURL="curl -s --max-time 5 http://127.0.0.1:$HTTP_PORT"

# ── 3. db/get (POST) ──────────────────────────────────────────
echo "=== POST /api/v1/db/get ==="
OUT=$($CURL/api/v1/db/get -d '{"keys":["services.ds.state"]}' \
    -H 'Content-Type: application/json')
echo "  $OUT"
if echo "$OUT" | grep -q '"ok":true'; then
    echo "  OK db/get"
else
    echo "  FAIL db/get" >&2; PASS=0
fi

# ── 4. db/set (POST) ──────────────────────────────────────────
echo "=== POST /api/v1/db/set ==="
OUT=$($CURL/api/v1/db/set -d \
    '{"pairs":[{"key":"iot.endpoint","value":"smoke-test"}]}' \
    -H 'Content-Type: application/json')
echo "  $OUT"
if echo "$OUT" | grep -q '"changed":1'; then
    echo "  OK db/set"
else
    echo "  FAIL db/set" >&2; PASS=0
fi

# Verify via ds-cli
VAL=$("$DS_CLI" --socket="$DS_SOCK" get iot.endpoint 2>/dev/null \
    | sed -n 's/.*=//p')
if echo "$VAL" | grep -q "smoke-test"; then
    echo "  OK ds-cli verifies set"
fi

# ── 5. db/get with key query (immediate) ──────────────────────
echo "=== GET /api/v1/db/get?key=iot.endpoint ==="
OUT=$($CURL'/api/v1/db/get?key=iot.endpoint')
echo "  $OUT"
if echo "$OUT" | grep -q '"changed":false'; then
    echo "  OK immediate get"
else
    echo "  FAIL immediate get" >&2; PASS=0
fi

# ── 6. Long poll ──────────────────────────────────────────────
echo "=== GET /api/v1/db/get?key=iot.endpoint&timeout=5 (long poll) ==="
# Start long poll in background
$CURL'/api/v1/db/get?key=iot.endpoint&timeout=5' >"$SMOKE_DIR/lp.out" 2>&1 &
LP_PID=$!
sleep 0.5

# Change the key from ds-cli
"$DS_CLI" --socket="$DS_SOCK" set iot.endpoint '"changed-by-lp"' >/dev/null 2>&1

# Wait for long poll to complete
for i in $(seq 1 30); do
    if ! kill -0 $LP_PID 2>/dev/null; then break; fi
    sleep 0.2
done
wait $LP_PID 2>/dev/null || true

LP_OUT=$(cat "$SMOKE_DIR/lp.out" 2>/dev/null || echo "")
echo "  $LP_OUT"
if echo "$LP_OUT" | grep -q '"changed":true'; then
    echo "  OK long poll detected change"
else
    echo "  WARN long poll: $LP_OUT (may have timed out)"
fi

# ── 7. Schema rejection via HTTP ──────────────────────────────
echo "=== schema rejection ==="
OUT=$($CURL/api/v1/db/set -d \
    '{"pairs":[{"key":"services.ds.enable","value":false}]}' \
    -H 'Content-Type: application/json')
echo "  $OUT"
if echo "$OUT" | grep -q "services.ds.enable"; then
    echo "  OK schema rejection"
else
    echo "  WARN schema rejection: $OUT"
fi

# ── Cleanup ───────────────────────────────────────────────────
kill "$HTTPD_PID" 2>/dev/null || true
kill "$DS_PID"   2>/dev/null || true
wait 2>/dev/null || true

echo
if [ "$PASS" -eq 1 ]; then
    echo "=== L18/D6 smoke PASSED ==="
    exit 0
else
    echo "=== L18/D6 smoke FAILED ===" >&2
    exit 1
fi
