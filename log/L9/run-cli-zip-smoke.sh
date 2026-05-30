#!/usr/bin/env bash
# Verify CoAPAdapter compress/uncompress round-trip via the CLI.
# CoAPAdapter::buildRequest deflates when the JSON-derived CBOR is
# >= 1024 bytes and chunks it across 1024-byte CoAP frames. This
# only fires from the `post`/`push`/`set`/`get`/`exec` commands when
# the user supplies `file=<path>` (the JSON literal `data=` arg path
# is capped at 1024 bytes by post_cmd / cli::build_payload, so it
# never reaches the compress branch).
#
# Plan:
#   1. Server + tcpdump on a podman network.
#   2. One-shot client that types `post uri=/push file=/tmp/big.json`
#      where /tmp/big.json holds a ~2.5 KB JSON object — well past
#      the 1024-byte deflate threshold.
#   3. Capture pcap; expect ≥1 deflated POST /push frame from the
#      client, and a server log line confirming uncompress ran.
#
# Usage:  bash log/L9/run-cli-zip-smoke.sh
set -euo pipefail

NET=lwm2m-zip-smoke
SRV=zip-server
CLI=zip-client
SNIFF=zip-sniff
PCAP=log/L9/cli-zip-smoke.pcap
PODMAN=/opt/homebrew/bin/podman
EP=urn:dev:zip-1

cleanup() {
    $PODMAN kill -s SIGKILL $SNIFF $CLI $SRV >/dev/null 2>&1 || true
    $PODMAN rm   -f         $SNIFF $CLI $SRV >/dev/null 2>&1 || true
    $PODMAN network rm -f   $NET             >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "[zip] (re)creating podman network $NET"
$PODMAN network rm -f $NET >/dev/null 2>&1 || true
$PODMAN network create $NET >/dev/null

echo "[zip] starting iot server on coap://0.0.0.0:5683"
$PODMAN run -d --rm --name $SRV --network $NET \
    -v "$PWD/apps/config":/opt/app/config \
    -v "$PWD/apps/build/lwm2m":/opt/app/lwm2m \
    naushada/iot:latest \
    /opt/app/lwm2m "local=coap://0.0.0.0:5683" "role=server" \
                   "ep=urn:dev:zip-server" "config=/opt/app/config" >/dev/null
sleep 2

echo "[zip] starting tcpdump sniffer"
$PODMAN run -d --rm --name $SNIFF --net=container:$SRV --user=root \
    --cap-add=NET_RAW -v "$PWD/log/L9":/cap \
    docker.io/nicolaka/netshoot:latest \
    tcpdump -i any -U -w /cap/cli-zip-smoke.pcap "udp port 5683" >/dev/null
sleep 2

# Synthesize a ~2.5 KB JSON on host (no nested-shell quoting hell)
# and bind-mount as /tmp/big.json in the client container.
BIG=log/L9/.cli-zip-big.json
{
    printf '{"items":['
    sep=''
    for i in $(seq 0 49); do
        printf '%s{"k%d":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}' "$sep" "$i"
        sep=','
    done
    printf ']}'
} >"$BIG"
echo "[zip] big.json size: $(wc -c <"$BIG") bytes (deflate kicks in at ≥1024)"

echo "[zip] running client with file= deflate payload"
$PODMAN run -d --name $CLI --network $NET \
    -v "$PWD/apps/config":/opt/app/config \
    -v "$PWD/apps/build/lwm2m":/opt/app/lwm2m \
    -v "$PWD/$BIG":/tmp/big.json:ro \
    --entrypoint /bin/bash \
    naushada/iot:latest \
    -c "
        {
            sleep 2; echo 'post uri=/push uri-query=ep=A12345 file=/tmp/big.json content-format=12201';
            sleep 4; echo 'quit';
            sleep 1;
        } | script -qc '/opt/app/lwm2m local=coap://0.0.0.0:56830 bs=coap://$SRV:5683 role=client ep=$EP config=/opt/app/config' /dev/null
    " >/dev/null

# Wait up to 15s for natural exit.
for i in $(seq 1 15); do
    if ! $PODMAN ps --format '{{.Names}}' | grep -q "^$CLI$"; then break; fi
    sleep 1
done
$PODMAN kill -s SIGKILL $CLI >/dev/null 2>&1 || true
$PODMAN logs $CLI 2>&1 >log/L9/cli-zip-smoke-client.log || true
$PODMAN logs $SRV 2>&1 >log/L9/cli-zip-smoke-server.log || true

echo "===== client (last 10 lines) ====="
tail -10 log/L9/cli-zip-smoke-client.log | sed 's/^/  /'

echo "===== server (uncompress markers) ====="
grep -E "uncompress|compress|coap_adapter.cpp:(1384|275|329)" log/L9/cli-zip-smoke-server.log | tail -20 | sed 's/^/  /'

echo "[zip] tearing down (pcap closed)"
$PODMAN kill $SNIFF $SRV >/dev/null 2>&1 || true
sleep 1

echo "[zip] pcap → $PCAP"
ls -la $PCAP

echo "[zip] wire frames:"
$PODMAN run --rm -v "$PWD/log/L9":/cap docker.io/nicolaka/netshoot:latest \
    tshark -r /cap/cli-zip-smoke.pcap -nn 2>/dev/null | sed 's/^/  /'
