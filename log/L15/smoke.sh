#!/bin/sh
# L15/D8 — end-to-end wifi-client smoke.
#
# Boots ds-server with wifi.lua loaded, seeds wifi.networks via
# ds-cli, runs wifi-client --once pointing at log/L15/fake-wpa.py
# in place of real wpa_supplicant. wifi-client spawns fake-wpa
# (so the NM-conflict guard is satisfied), connects to the ctrl
# socket fake-wpa binds, sends SCAN, and walks scanning -> connected
# off the canned CTRL-EVENT sequence the fake emits.
#
# Real wpa_supplicant + a real radio is out of scope (no AP to
# test against, rootless podman can't talk to the host wlan stack).
# The parser + lifecycle FSM are exhaustively unit-tested with the
# canned event traces at modules/wan/wifi/client/test/data/.
#
# Run from the repo root, or via podman:
#   podman run --rm -v $PWD:/work -w /work naushada/iot:latest \
#     bash -c 'apt-get install -y -qq liblua5.3-dev python3 2>&1|tail -1; sh log/L15/smoke.sh'

set -eu

REPO=$(pwd)
SMOKE_DIR=/tmp/wifi-smoke
DS_SOCK="$SMOKE_DIR/ds.sock"
DS_STORE="$SMOKE_DIR/store.lua"
SCHEMA_DIR="$SMOKE_DIR/schemas"
CTRL_DIR="$SMOKE_DIR/wpa-ctrl"
IFACE=wlan-smoke
WIFI_LOG="$SMOKE_DIR/wifi-client.log"
TRANSCRIPT="$REPO/log/L15/wifi-smoke.txt"

# Locate binaries — module builds first, fall back to /usr/local/bin
# in case the smoke runs against an installed image.
DS_SERVER="${DS_SERVER:-$REPO/modules/data-store/build/ds-server}"
DS_CLI="${DS_CLI:-$REPO/modules/data-store/build/ds-cli}"
WIFI_CLIENT="${WIFI_CLIENT:-$REPO/modules/wan/wifi/client/build/wifi-client}"
FAKE_WPA="$REPO/log/L15/fake-wpa.py"
[ -x "$DS_SERVER" ]   || DS_SERVER=/usr/local/bin/ds-server
[ -x "$DS_CLI" ]      || DS_CLI=/usr/local/bin/ds-cli
[ -x "$WIFI_CLIENT" ] || WIFI_CLIENT=/usr/local/bin/wifi-client

cleanup() {
    pkill -P $$ 2>/dev/null || true
    rm -rf "$SMOKE_DIR"
}
trap cleanup EXIT INT TERM
cleanup

mkdir -p "$SMOKE_DIR" "$SCHEMA_DIR" "$CTRL_DIR"
cp "$REPO/modules/data-store/schemas/iot.lua"     "$SCHEMA_DIR/"
cp "$REPO/modules/wan/wifi/client/schemas/wifi.lua" "$SCHEMA_DIR/"

# ── 1. ds-server with wifi.lua loaded ──────────────────────────────
"$DS_SERVER" \
    ds-socket="$DS_SOCK" \
    ds-store="$DS_STORE" \
    ds-schema-dir="$SCHEMA_DIR" \
    >"$SMOKE_DIR/ds.log" 2>&1 &
DS_PID=$!
sleep 0.5

# Seed required wifi.* keys via ds-cli.
"$DS_CLI" --socket="$DS_SOCK" set wifi.iface     '"'"$IFACE"'"'        >/dev/null
"$DS_CLI" --socket="$DS_SOCK" set wifi.ctrl.dir  '"'"$CTRL_DIR"'"'     >/dev/null
"$DS_CLI" --socket="$DS_SOCK" set wifi.networks \
    '[{"ssid":"HomeAP","psk":"correcthorse","priority":10}]'         >/dev/null
echo "OK ds-server up; wifi.* seeded (iface=$IFACE)"

# ── 2. wifi-client --once  (spawns fake-wpa via --wpa) ─────────────
# wifi-client's Supervisor calls spawn_wpa_supplicant which exec's
# our fake-wpa.py with wpa_supplicant-style -i / -c / -C argv.
# The fake binds the ctrl socket, accepts ATTACH, then walks the
# scanning -> SCAN-RESULTS -> CONNECTED event sequence when the
# Supervisor issues SCAN. --once tells the daemon to return after
# the first CONNECTED event.
"$WIFI_CLIENT" \
    --ds-sock="$DS_SOCK" \
    --wpa="$FAKE_WPA" \
    --iface="$IFACE" \
    --ctrl-dir="$CTRL_DIR" \
    --once \
    >"$WIFI_LOG" 2>&1 &
WIFI_PID=$!

# Wait up to 10s for wifi.assoc.state to reach "connected".
deadline=$(( $(date +%s) + 10 ))
state=""
while [ "$(date +%s)" -lt "$deadline" ]; do
    state=$("$DS_CLI" --socket="$DS_SOCK" get wifi.assoc.state 2>/dev/null \
              | sed -n 's/^wifi\.assoc\.state=//p')
    [ "$state" = "connected" ] && break
    sleep 0.2
done

# ── 3. Final state read ────────────────────────────────────────────
echo
echo "=== final wifi.* snapshot ==="
"$DS_CLI" --socket="$DS_SOCK" get \
    wifi.assoc.state wifi.assoc.ssid wifi.assoc.bssid \
    wifi.scan.results wifi.last.error 2>&1

# ── 4. Assert + capture transcript ─────────────────────────────────
PASS=1
if [ "$state" = "connected" ]; then
    echo "OK wifi.assoc.state = connected"
else
    echo "FAIL wifi.assoc.state = '$state' (expected 'connected')" >&2
    PASS=0
fi

SSID=$("$DS_CLI" --socket="$DS_SOCK" get wifi.assoc.ssid 2>/dev/null \
         | sed -n 's/^wifi\.assoc\.ssid=//p')
if [ -n "$SSID" ] && [ "$SSID" != "(null)" ]; then
    echo "OK wifi.assoc.ssid = $SSID"
else
    echo "WARN wifi.assoc.ssid empty (fake-wpa id_str path is informational)" >&2
fi

# REQ-WIFI-025: wifi.scan.results MUST be non-empty JSON.
RESULTS=$("$DS_CLI" --socket="$DS_SOCK" get wifi.scan.results 2>/dev/null \
            | sed -n 's/^wifi\.scan\.results=//p')
case "$RESULTS" in
    ""|"(null)"|"[]")
        echo "FAIL wifi.scan.results = '$RESULTS' (expected non-empty JSON)" >&2
        PASS=0
        ;;
    *)
        echo "OK wifi.scan.results = $RESULTS"
        ;;
esac

# Write the transcript before exiting (transcript wanted regardless
# of pass/fail so a CI failure carries the evidence).
{
    echo "L15/D8 smoke transcript $(date -u '+%Y-%m-%d %H:%M:%S')Z"
    echo "==========================================="
    echo "ds.log:"
    sed 's/^/  /' "$SMOKE_DIR/ds.log" 2>/dev/null
    echo
    echo "wifi-client.log:"
    sed 's/^/  /' "$WIFI_LOG"
    echo
    echo "final state:"
    "$DS_CLI" --socket="$DS_SOCK" get \
        wifi.assoc.state wifi.assoc.ssid wifi.assoc.bssid \
        wifi.scan.results wifi.last.error 2>/dev/null | sed 's/^/  /'
} > "$TRANSCRIPT"
echo
echo "transcript: $TRANSCRIPT"

wait "$WIFI_PID" 2>/dev/null || true
kill "$DS_PID" 2>/dev/null || true

if [ "$PASS" -eq 1 ]; then
    echo "=== L15/D8 smoke PASSED ==="
    exit 0
else
    echo "=== L15/D8 smoke FAILED ===" >&2
    exit 1
fi
