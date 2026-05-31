#!/bin/sh
# Fake openvpn(8) for the L14 end-to-end smoke.
#
# Adapted from log/L12/openvpn-smoke.sh's inline fake, with one
# behaviour change: instead of replaying the canned mgmt stream
# once and exiting, this fake stays alive until killed so the
# openvpn-client daemon (running WITHOUT --once) sees a persistent
# tunnel. The compose-up path runs openvpn-client as a service,
# not a one-shot.
#
# argv we get from openvpn-client: --config /path/to/conf
#                                   --management 127.0.0.1 <port>
#                                   --daemon -- and friends
# We only care about --management <addr> <port>.

set -eu

PORT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --management) PORT="$3"; shift 3 ;;
        *)            shift ;;
    esac
done

if [ -z "$PORT" ]; then
    echo "fake-openvpn: no --management <addr> <port> in argv" >&2
    exit 64
fi

# Recorder: write our argv to a host-visible file so the smoke can
# assert "openvpn-client really spawned us with the expected port".
RECORDER_DIR="${FAKE_RECORDER_DIR:-/recorder}"
mkdir -p "$RECORDER_DIR"
printf 'invoked port=%s pid=%s at=%s\n' "$PORT" "$$" "$(date -u +%FT%TZ)" \
    >> "$RECORDER_DIR/fake-openvpn.log"

# Spool a canned mgmt stream into the mgmt-socket peer (openvpn-client).
# nc -lN means "listen, exit when peer closes" — but we want this fake
# to hang forever so the daemon's lifecycle stays in `running`. Use a
# FIFO + tail -f to keep the listener pipe open after the canned
# payload is drained.
SPOOL=$(mktemp -u "${TMPDIR:-/tmp}/fake-ovpn-spool.XXXXXX")
mkfifo "$SPOOL"
trap 'rm -f "$SPOOL"' EXIT

(
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
    # Stay open: tail -f /dev/null keeps the pipe alive so the listener
    # nc doesn't EOF us out.
    tail -f /dev/null
) > "$SPOOL" &
SPOOLER_PID=$!

# Use nc-openbsd (which is what the dev/runtime image ships via
# `netcat-openbsd`). -k = keep listening across peer disconnects.
nc -lk 127.0.0.1 "$PORT" < "$SPOOL" &
NC_PID=$!

# Reap children on signal.
trap 'kill $SPOOLER_PID $NC_PID 2>/dev/null || true; rm -f "$SPOOL"; exit 0' \
    TERM INT HUP

wait $NC_PID
