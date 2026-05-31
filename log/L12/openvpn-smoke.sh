#!/bin/sh
# L12/D6 — end-to-end openvpn-client smoke.
#
# Boots ds-server, populates vpn.* required keys, then runs
# openvpn-client with --once against a FAKE openvpn (a shell script
# that listens on the mgmt port and emits canned >STATE: + >PUSH_REPLY:
# lines). Real openvpn(8) needs a tun device + CAP_NET_ADMIN + a real
# server peer — outside what a CI smoke can sanely provide. The
# wire-level lifecycle code is what matters; the parser + state FSM
# are exhaustively unit-tested with realistic fixtures.
#
# Run from the repo root:
#   sh log/L12/openvpn-smoke.sh
# or via podman:
#   podman run --rm -v $PWD:/work -w /work naushada/iot:latest \
#     bash -c 'apt-get install -y -qq liblua5.3-dev netcat-openbsd 2>&1|tail -1; sh log/L12/openvpn-smoke.sh'

set -eu

DS_SOCK=/tmp/ovpn-smoke/ds.sock
DS_STORE=/tmp/ovpn-smoke/store.lua
SCHEMA_DIR=/tmp/ovpn-smoke/schemas
MGMT_PORT=17505
FAKE_OPENVPN=/tmp/ovpn-smoke/fake-openvpn

# Locate the binaries built either by the module build or the bare-
# metal install.
DS_SERVER="${DS_SERVER:-./modules/data-store/build/ds-server}"
DS_CLI="${DS_CLI:-./modules/data-store/build/ds-cli}"
OPENVPN_CLIENT="${OPENVPN_CLIENT:-./modules/openvpn/client/build/openvpn-client}"

cleanup() {
    pkill -P $$ 2>/dev/null || true
    rm -rf /tmp/ovpn-smoke
}
trap cleanup EXIT INT TERM
cleanup

mkdir -p /tmp/ovpn-smoke "$SCHEMA_DIR"
cp modules/data-store/schemas/iot.lua  "$SCHEMA_DIR/"
cp modules/openvpn/client/schemas/vpn.lua "$SCHEMA_DIR/"

# ── 1. Fake openvpn: binds the mgmt port + replays a captured stream
#       that walks CONNECTING → CONNECTED, then idles until killed.
cat > "$FAKE_OPENVPN" <<'EOF'
#!/bin/sh
# Args we receive: --config /tmp/openvpn-XXX.conf --management 127.0.0.1 <port>
# We only care about the port.
PORT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --management) PORT="$3"; shift 3 ;;
        *) shift ;;
    esac
done

# Echo a canned mgmt stream over a one-shot nc listener. The client
# connects → reads these lines → exits (in --once mode).
{
    printf '>INFO:OpenVPN Management Interface Version 1 -- type help for more info\r\n'
    sleep 0.1
    printf '>STATE:1,CONNECTING,,,,,\r\n'
    sleep 0.1
    printf '>STATE:2,WAIT,,,,,\r\n'
    sleep 0.1
    printf '>STATE:3,AUTH,,,,,\r\n'
    sleep 0.1
    printf '>STATE:4,GET_CONFIG,,,,,\r\n'
    sleep 0.1
    printf '>PUSH_REPLY:dhcp-option DNS 1.1.1.1,route-gateway 10.8.0.1,ifconfig 10.8.0.99 255.255.255.0\r\n'
    sleep 0.1
    printf '>STATE:5,ASSIGN_IP,,10.8.0.99,,,,,\r\n'
    sleep 0.1
    printf '>STATE:6,CONNECTED,SUCCESS,10.8.0.99,vpn.example.com,1194,,\r\n'
    # Idle until our peer disconnects.
    sleep 30
} | nc -lN 127.0.0.1 "$PORT"
EOF
chmod +x "$FAKE_OPENVPN"

# ── 2. Boot ds-server
"$DS_SERVER" ds-socket="$DS_SOCK" ds-store="$DS_STORE" ds-schema-dir="$SCHEMA_DIR" >/dev/null 2>&1 &
sleep 0.3
"$DS_CLI" --socket="$DS_SOCK" set vpn.remote.host '"10.0.0.5"'           >/dev/null
"$DS_CLI" --socket="$DS_SOCK" set vpn.cert.path   '"/etc/vpn/c.crt"'     >/dev/null
"$DS_CLI" --socket="$DS_SOCK" set vpn.key.path    '"/etc/vpn/c.key"'     >/dev/null
"$DS_CLI" --socket="$DS_SOCK" set vpn.ca.path     '"/etc/vpn/ca.crt"'    >/dev/null
"$DS_CLI" --socket="$DS_SOCK" set vpn.mgmt.port   "$MGMT_PORT"           >/dev/null
echo "OK ds-server up + required vpn.* keys seeded"

# ── 3. Run openvpn-client with --once + fake openvpn
echo "=== openvpn-client --once output ==="
"$OPENVPN_CLIENT" \
    --ds-sock="$DS_SOCK" \
    --openvpn="$FAKE_OPENVPN" \
    --once \
    2>&1 | grep -E "spawned|PUSH_REPLY|tunnel|state|exited" || true

# ── 4. Verify ds-server cache reflects the pushed values
echo
echo "=== ds-cli get vpn.assigned.* + vpn.state ==="
"$DS_CLI" --socket="$DS_SOCK" get \
    vpn.state vpn.assigned.ip vpn.assigned.gateway \
    vpn.assigned.netmask vpn.assigned.dns

# ── 5. Assert
EXPECTED_IP="10.8.0.99"
GOT_IP=$("$DS_CLI" --socket="$DS_SOCK" get vpn.assigned.ip | sed -n 's/^vpn\.assigned\.ip=//p')
if [ "$GOT_IP" = "$EXPECTED_IP" ]; then
    echo "OK vpn.assigned.ip = $GOT_IP (matches fake openvpn's pushed value)"
else
    echo "FAIL vpn.assigned.ip = '$GOT_IP' (expected '$EXPECTED_IP')" >&2
    exit 1
fi

echo
echo "=== Summary: D6 lifecycle smoke passed ==="
