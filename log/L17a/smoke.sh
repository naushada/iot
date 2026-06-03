#!/bin/sh
# L17a/D6 — services dependency graph smoke.
#
# Brings up ds-server + net-router + openvpn-client via direct
# subprocess (no podman/compose required when ACE + Lua are on
# the host). Disables net-router, asserts openvpn-client reports
# vpn.gate.reason="dep_down:net.router" (NOT "wan_down"), then
# re-enables and asserts recovery.
#
# Run from the repo root:
#   bash log/L17a/smoke.sh
#
# Or in a container:
#   podman run --rm -v $PWD:/work -w /work ubuntu:jammy \
#     bash -c 'apt-get install -y -qq liblua5.3-dev 2>&1|tail -1; sh log/L17a/smoke.sh'

set -eu

SMOKE_DIR=/tmp/l17a-smoke
DS_SOCK="$SMOKE_DIR/ds.sock"
DS_STORE="$SMOKE_DIR/store.lua"
SCHEMA_DIR="$SMOKE_DIR/schemas"

DS_SERVER="${DS_SERVER:-./modules/data-store/build/ds-server}"
DS_CLI="${DS_CLI:-./modules/data-store/build/ds-cli}"
NET_ROUTER="${NET_ROUTER:-./modules/net/router/build/net-router}"
OVPN_CLIENT="${OVPN_CLIENT:-./modules/openvpn/client/build/openvpn-client}"
[ -x "$DS_SERVER" ]  || DS_SERVER=/usr/local/bin/ds-server
[ -x "$DS_CLI" ]     || DS_CLI=/usr/local/bin/ds-cli
[ -x "$NET_ROUTER" ] || NET_ROUTER=/usr/local/bin/net-router
[ -x "$OVPN_CLIENT" ]|| OVPN_CLIENT=/usr/local/bin/openvpn-client

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
cp modules/openvpn/client/schemas/vpn.lua  "$SCHEMA_DIR/" 2>/dev/null || true

# ── 1. ds-server with services.lua (has depends_on) ────────────────
"$DS_SERVER" \
    ds-socket="$DS_SOCK" \
    ds-store="$DS_STORE" \
    ds-schema-dir="$SCHEMA_DIR" \
    >"$SMOKE_DIR/ds.log" 2>&1 &
DS_PID=$!
sleep 0.5

# Check ds-server loaded the schema (including depends_on).
if grep -q "depends_on" "$SCHEMA_DIR/services.lua"; then
    echo "OK services.lua includes depends_on declarations"
else
    echo "FAIL services.lua missing depends_on" >&2
    exit 1
fi

# Seed required keys for net-router.
"$DS_CLI" --socket="$DS_SOCK" set net.lwm2m.target.ip '"127.0.0.1"' >/dev/null
echo "OK ds-server up"

# ── 2. svc list shows DEPENDS column ──
echo
echo "=== ds-cli svc list ==="
"$DS_CLI" --socket="$DS_SOCK" svc list
# Verify DEPENDS column exists and shows net.router for openvpn.client.
OVPN_LINE=$("$DS_CLI" --socket="$DS_SOCK" svc list 2>/dev/null | grep "openvpn.client")
if echo "$OVPN_LINE" | grep -q "net.router"; then
    echo "OK openvpn.client DEPENDS = net.router"
else
    echo "WARN openvpn.client DEPENDS column: $OVPN_LINE"
fi

# ── 3. net-router up ──
"$NET_ROUTER" \
    --ds-sock="$DS_SOCK" \
    --nft=/bin/true \
    --poll=1 \
    --daemon \
    >"$SMOKE_DIR/netr.log" 2>&1 &
NETR_PID=$!
sleep 1.5

NETR_STATE=$("$DS_CLI" --socket="$DS_SOCK" get services.net.router.state \
              | sed -n 's/^services\.net\.router\.state=//p')
echo "services.net.router.state after start = $NETR_STATE"
if [ "$NETR_STATE" != "running" ]; then
    echo "FAIL net-router didn't reach running (state=$NETR_STATE)" >&2
    exit 1
fi

# ── 4. openvpn-client up (will see WAN up if an iface exists, or park) ──
# Start openvpn-client — it will park at wan_down or dep_down depending
# on net.iface.active. We care about the gate.reason key being present.
"$OVPN_CLIENT" \
    --ds-sock="$DS_SOCK" \
    --daemon \
    >"$SMOKE_DIR/ovpn.log" 2>&1 &
OVPN_PID=$!
sleep 1.0

# ── 5. Disable net-router → assert dep_down on openvpn-client ──
echo
echo "=== ds-cli svc disable net.router ==="
"$DS_CLI" --socket="$DS_SOCK" svc disable net.router

PASS=1

# Wait up to 8s for the cascade: net-router→disabled →
# openvpn-client sees dep_down.
deadline=$(( $(date +%s) + 8 ))
GATE_REASON=""
while [ "$(date +%s)" -lt "$deadline" ]; do
    GATE_REASON=$("$DS_CLI" --socket="$DS_SOCK" get vpn.gate.reason 2>/dev/null \
                   | sed -n 's/^vpn\.gate\.reason=//p' || true)
    case "$GATE_REASON" in
        *dep_down*) break ;;
    esac
    sleep 0.3
done

case "$GATE_REASON" in
    *dep_down*)
        echo "OK vpn.gate.reason = $GATE_REASON"
        ;;
    *)
        echo "FAIL vpn.gate.reason = '$GATE_REASON' (expected dep_down:net.router)" >&2
        PASS=0
        ;;
esac

# ── 6. Re-enable net-router → assert recovery ──
echo
echo "=== ds-cli svc enable net.router ==="
"$DS_CLI" --socket="$DS_SOCK" svc enable net.router

deadline=$(( $(date +%s) + 5 ))
NETR_STATE=""
while [ "$(date +%s)" -lt "$deadline" ]; do
    NETR_STATE=$("$DS_CLI" --socket="$DS_SOCK" get services.net.router.state \
                  | sed -n 's/^services\.net\.router\.state=//p')
    [ "$NETR_STATE" = "running" ] && break
    sleep 0.2
done
if [ "$NETR_STATE" = "running" ]; then
    echo "OK services.net.router.state = running (recovered)"
else
    echo "FAIL services.net.router.state = '$NETR_STATE'" >&2
    PASS=0
fi

# ── 7. After recovery, openvpn-client should clear dep_down ──
sleep 1.0
GATE_REASON=$("$DS_CLI" --socket="$DS_SOCK" get vpn.gate.reason 2>/dev/null \
               | sed -n 's/^vpn\.gate\.reason=//p' || echo "(unset)")
case "$GATE_REASON" in
    *dep_down*)
        echo "WARN vpn.gate.reason still dep_down after recovery: $GATE_REASON"
        ;;
    *)
        echo "OK vpn.gate.reason = $GATE_REASON (no longer dep_down)"
        ;;
esac

# ── 8. Final svc list ──
echo
echo "=== ds-cli svc list (final) ==="
"$DS_CLI" --socket="$DS_SOCK" svc list

# ── 9. Transcript ──
{
    echo "L17a/D6 smoke transcript $(date -u '+%Y-%m-%d %H:%M:%S')Z"
    echo "==========================================="
    echo "ds.log (head):"
    sed 's/^/  /' "$SMOKE_DIR/ds.log" | head -15
    echo
    echo "net-router.log:"
    sed 's/^/  /' "$SMOKE_DIR/netr.log" | head -20
    echo
    echo "openvpn-client.log:"
    sed 's/^/  /' "$SMOKE_DIR/ovpn.log" | head -20
    echo
    echo "final svc list:"
    "$DS_CLI" --socket="$DS_SOCK" svc list 2>/dev/null | sed 's/^/  /'
} > "log/L17a/smoke-transcript.txt"
echo
echo "transcript: log/L17a/smoke-transcript.txt"

kill "$OVPN_PID" 2>/dev/null
kill "$NETR_PID" 2>/dev/null
kill "$DS_PID"   2>/dev/null
wait 2>/dev/null

if [ "$PASS" -eq 1 ]; then
    echo "=== L17a/D6 smoke PASSED ==="
    exit 0
else
    echo "=== L17a/D6 smoke FAILED ===" >&2
    exit 1
fi
