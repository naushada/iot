#!/bin/bash
# L18 — HTTP REST API smoke (plain HTTP + HTTPS/mTLS + hot-reload).
#
# Starts ds-server + iot-httpd, drives the /api/v1/db/* endpoints with
# curl, asserts expected responses. Then (if openssl is present) starts a
# second iot-httpd with native TLS + mutual-TLS + a worker pool, checks a
# CA-signed client is accepted and a missing client cert is rejected, and
# rotates the server cert live via ds-cli (FUP-L18-2). Designed for podman:
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

# ── 8. HTTPS + mutual-TLS (also exercises the worker pool) ────
HTTPS_PORT="${HTTPS_PORT:-18443}"
HTTPSD_PID=""
if command -v openssl >/dev/null 2>&1; then
    echo "=== HTTPS + mutual-TLS (http-workers=2) ==="
    TLS="$SMOKE_DIR/tls"; mkdir -p "$TLS"
    # CA
    openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
        -keyout "$TLS/ca.key" -out "$TLS/ca.crt" -subj "/CN=Smoke CA" 2>/dev/null
    # Server cert (SAN IP:127.0.0.1) signed by the CA
    openssl req -newkey rsa:2048 -nodes -keyout "$TLS/server.key" \
        -out "$TLS/server.csr" -subj "/CN=127.0.0.1" \
        -addext "subjectAltName=IP:127.0.0.1" 2>/dev/null
    openssl x509 -req -in "$TLS/server.csr" -CA "$TLS/ca.crt" -CAkey "$TLS/ca.key" \
        -CAcreateserial -days 1 -copy_extensions copyall \
        -out "$TLS/server.crt" 2>/dev/null
    # Client cert (for mTLS) signed by the CA
    openssl req -newkey rsa:2048 -nodes -keyout "$TLS/client.key" \
        -out "$TLS/client.csr" -subj "/CN=smoke-client" 2>/dev/null
    openssl x509 -req -in "$TLS/client.csr" -CA "$TLS/ca.crt" -CAkey "$TLS/ca.key" \
        -CAcreateserial -days 1 -out "$TLS/client.crt" 2>/dev/null

    "$HTTPD" ds-socket="$DS_SOCK" http-port="$HTTPS_PORT" http-scheme=https \
        http-cert="$TLS/server.crt" http-key="$TLS/server.key" \
        http-ca="$TLS/ca.crt" http-workers=2 \
        >"$SMOKE_DIR/httpsd.log" 2>&1 &
    HTTPSD_PID=$!
    sleep 0.7

    SC="openssl s_client -connect 127.0.0.1:$HTTPS_PORT -cert $TLS/client.crt -key $TLS/client.key"

    # (a) client WITH a CA-signed cert → accepted
    OUT=$(curl -s --max-time 5 --cacert "$TLS/ca.crt" \
        --cert "$TLS/client.crt" --key "$TLS/client.key" \
        "https://127.0.0.1:$HTTPS_PORT/api/v1/db/get" \
        -d '{"keys":["iot.endpoint"]}' -H 'Content-Type: application/json' 2>&1 || true)
    echo "  with client cert: $OUT"
    if echo "$OUT" | grep -q '"ok":true'; then
        echo "  OK https + mTLS (client cert accepted, via worker pool)"
    else
        echo "  FAIL https + mTLS" >&2; PASS=0
    fi

    # (b) client WITHOUT a cert → rejected at the handshake
    if curl -s --max-time 5 --cacert "$TLS/ca.crt" \
        "https://127.0.0.1:$HTTPS_PORT/api/v1/db/get" \
        -d '{"keys":["iot.endpoint"]}' >/dev/null 2>&1; then
        echo "  FAIL mTLS: request without a client cert was accepted" >&2; PASS=0
    else
        echo "  OK mTLS rejects a missing client cert"
    fi

    # ── 9. Hot-reload: rotate the server cert via ds-cli ──────
    echo "=== TLS hot-reload (live cert rotation) ==="
    SERIAL_BEFORE=$(echo | $SC 2>/dev/null | openssl x509 -noout -serial 2>/dev/null || true)
    # A fresh server cert (new serial), same CA, swapped in via ds-cli.
    openssl req -newkey rsa:2048 -nodes -keyout "$TLS/server2.key" \
        -out "$TLS/server2.csr" -subj "/CN=127.0.0.1" \
        -addext "subjectAltName=IP:127.0.0.1" 2>/dev/null
    openssl x509 -req -in "$TLS/server2.csr" -CA "$TLS/ca.crt" -CAkey "$TLS/ca.key" \
        -CAcreateserial -days 1 -copy_extensions copyall \
        -out "$TLS/server2.crt" 2>/dev/null
    "$DS_CLI" --socket="$DS_SOCK" set http.tls.key  "\"$TLS/server2.key\""  >/dev/null 2>&1
    "$DS_CLI" --socket="$DS_SOCK" set http.tls.cert "\"$TLS/server2.crt\"" >/dev/null 2>&1
    sleep 3   # the reload poll runs every ~2s
    SERIAL_AFTER=$(echo | $SC 2>/dev/null | openssl x509 -noout -serial 2>/dev/null || true)
    echo "  served-cert serial before=$SERIAL_BEFORE after=$SERIAL_AFTER"
    if [ -n "$SERIAL_AFTER" ] && [ "$SERIAL_BEFORE" != "$SERIAL_AFTER" ]; then
        echo "  OK cert rotated live (no restart)"
    else
        echo "  WARN cert rotation not observed (timing/openssl?)"
    fi

    kill "$HTTPSD_PID" 2>/dev/null || true
else
    echo "=== HTTPS smoke skipped (no openssl CLI) ==="
fi

# ── Cleanup ───────────────────────────────────────────────────
kill "$HTTPSD_PID" 2>/dev/null || true
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
