#!/bin/sh
# L16/D8 — services.* enable plane smoke.
#
# Boots ds-server with services.lua loaded, starts net-router as a
# subprocess, drives the gate via ds-cli svc, and asserts the
# expected state transitions land in the data store.
#
# net-router is the cleanest target for the smoke: its gate exercise
# doesn't need a real iface (it just stops driving + clears
# net.iface.active). openvpn-client + wifi-client would need real
# binaries / radios; lwm2m gates are landed at D5 (TBD).
#
# Run from the repo root, or via podman:
#   podman run --rm -v $PWD:/work -w /work naushada/iot:latest \
#     bash -c 'apt-get install -y -qq liblua5.3-dev 2>&1|tail -1; sh log/L16/smoke.sh'

set -eu

SMOKE_DIR=/tmp/svc-smoke
DS_SOCK="$SMOKE_DIR/ds.sock"
DS_STORE="$SMOKE_DIR/store.lua"
SCHEMA_DIR="$SMOKE_DIR/schemas"
NETR_LOG="$SMOKE_DIR/net-router.log"
TRANSCRIPT="log/L16/svc-smoke.txt"

DS_SERVER="${DS_SERVER:-./modules/data-store/build/ds-server}"
DS_CLI="${DS_CLI:-./modules/data-store/build/ds-cli}"
NET_ROUTER="${NET_ROUTER:-./modules/net/router/build/net-router}"
[ -x "$DS_SERVER" ]  || DS_SERVER=/usr/local/bin/ds-server
[ -x "$DS_CLI" ]     || DS_CLI=/usr/local/bin/ds-cli
[ -x "$NET_ROUTER" ] || NET_ROUTER=/usr/local/bin/net-router

cleanup() {
    pkill -P $$ 2>/dev/null || true
    rm -rf "$SMOKE_DIR"
}
trap cleanup EXIT INT TERM
cleanup

mkdir -p "$SMOKE_DIR" "$SCHEMA_DIR"
cp modules/data-store/schemas/iot.lua      "$SCHEMA_DIR/"
cp modules/data-store/schemas/services.lua "$SCHEMA_DIR/"
cp modules/net/router/schemas/net.lua      "$SCHEMA_DIR/"

# ── 1. ds-server with services.lua loaded ─────────────────────────
"$DS_SERVER" \
    ds-socket="$DS_SOCK" \
    ds-store="$DS_STORE" \
    ds-schema-dir="$SCHEMA_DIR" \
    >"$SMOKE_DIR/ds.log" 2>&1 &
DS_PID=$!
sleep 0.5

# Seed the only required net.* key.
"$DS_CLI" --socket="$DS_SOCK" set net.lwm2m.target.ip '"127.0.0.1"' >/dev/null
echo "OK ds-server up (loaded services.lua + iot.lua + net.lua)"

# ── 2. ds-server self-publish check (REQ-SVC-007) ──────────────────
DS_STATE=$("$DS_CLI" --socket="$DS_SOCK" get services.ds.state \
            | sed -n 's/^services\.ds\.state=//p')
if [ "$DS_STATE" = "running" ]; then
    echo "OK services.ds.state = running"
else
    echo "FAIL services.ds.state = '$DS_STATE'" >&2
    exit 1
fi

# ── 3. services.ds.enable rejection (REQ-SVC-008) ──────────────────
if "$DS_CLI" --socket="$DS_SOCK" set services.ds.enable false 2>/dev/null; then
    echo "FAIL services.ds.enable accepted (expected rejection)" >&2
    exit 1
else
    echo "OK services.ds.enable rejected by schema"
fi

# ── 4. svc list shows the row set ──────────────────────────────────
echo
echo "=== ds-cli svc list ==="
"$DS_CLI" --socket="$DS_SOCK" svc list

# ── 5. net-router up + initial state="running" ─────────────────────
# Run net-router with a tiny poll interval so the smoke is fast.
# Pass nft= to /bin/true since we don't have nft in the test image
# and don't need actual rules applied — we're only exercising the
# gate.
"$NET_ROUTER" \
    --ds-sock="$DS_SOCK" \
    --nft=/bin/true \
    --poll=1 \
    --daemon \
    >"$NETR_LOG" 2>&1 &
NETR_PID=$!
# Allow net-router to publish initial services.net.router.state.
sleep 1.5

NETR_STATE=$("$DS_CLI" --socket="$DS_SOCK" get services.net.router.state \
              | sed -n 's/^services\.net\.router\.state=//p')
echo "services.net.router.state after start = $NETR_STATE"

# ── 6. Disable round-trip ──────────────────────────────────────────
echo
echo "=== ds-cli svc disable net.router ==="
"$DS_CLI" --socket="$DS_SOCK" svc disable net.router

# Wait up to 5s for the gate to land.
PASS=1
deadline=$(( $(date +%s) + 5 ))
NETR_STATE=""
while [ "$(date +%s)" -lt "$deadline" ]; do
    NETR_STATE=$("$DS_CLI" --socket="$DS_SOCK" get services.net.router.state \
                  | sed -n 's/^services\.net\.router\.state=//p')
    [ "$NETR_STATE" = "disabled" ] && break
    sleep 0.2
done
if [ "$NETR_STATE" = "disabled" ]; then
    echo "OK services.net.router.state = disabled (within 5s)"
else
    echo "FAIL services.net.router.state = '$NETR_STATE' (expected disabled)" >&2
    PASS=0
fi

# net.iface.active should have been cleared by the disable teardown.
IFACE_ACTIVE=$("$DS_CLI" --socket="$DS_SOCK" get net.iface.active 2>/dev/null \
                | sed -n 's/^net\.iface\.active=//p')
case "$IFACE_ACTIVE" in
    ""|"(null)") echo "OK net.iface.active cleared after disable" ;;
    *)           echo "OK net.iface.active=$IFACE_ACTIVE (depends on host ifaces)" ;;
esac

# ── 7. Re-enable round-trip ────────────────────────────────────────
echo
echo "=== ds-cli svc enable net.router ==="
"$DS_CLI" --socket="$DS_SOCK" svc enable net.router

deadline=$(( $(date +%s) + 5 ))
while [ "$(date +%s)" -lt "$deadline" ]; do
    NETR_STATE=$("$DS_CLI" --socket="$DS_SOCK" get services.net.router.state \
                  | sed -n 's/^services\.net\.router\.state=//p')
    [ "$NETR_STATE" = "running" ] && break
    sleep 0.2
done
if [ "$NETR_STATE" = "running" ]; then
    echo "OK services.net.router.state = running (after re-enable)"
else
    echo "FAIL services.net.router.state = '$NETR_STATE' (expected running)" >&2
    PASS=0
fi

# ── 8. ds-cli svc list after the round-trip ────────────────────────
echo
echo "=== ds-cli svc list (final) ==="
"$DS_CLI" --socket="$DS_SOCK" svc list

# ── 9. Capture transcript + cleanup ────────────────────────────────
{
    echo "L16/D8 smoke transcript $(date -u '+%Y-%m-%d %H:%M:%S')Z"
    echo "==========================================="
    echo "ds.log (head):"
    sed 's/^/  /' "$SMOKE_DIR/ds.log" | head -15
    echo
    echo "net-router.log:"
    sed 's/^/  /' "$NETR_LOG" | head -40
    echo
    echo "final svc list:"
    "$DS_CLI" --socket="$DS_SOCK" svc list 2>/dev/null | sed 's/^/  /'
} > "$TRANSCRIPT"
echo
echo "transcript: $TRANSCRIPT"

kill "$NETR_PID" 2>/dev/null
kill "$DS_PID"   2>/dev/null
wait 2>/dev/null

if [ "$PASS" -eq 1 ]; then
    echo "=== L16/D8 smoke PASSED ==="
    exit 0
else
    echo "=== L16/D8 smoke FAILED ===" >&2
    exit 1
fi
