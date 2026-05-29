#!/usr/bin/env bash
# NFR-INTEROP-002 runner: wakaama lwm2mclient (a Leshan-class compliant
# LwM2M Client) ↔ our binary in server mode, plain CoAP. Captures a
# pcap to log/L9/nfr-002-coap.pcap.
#
# Usage:  bash log/L9/run-interop-002.sh
#
# Why wakaama and not the Leshan client?  Docker Hub has no canonical
# leshan-client-demo image; the wakaama lwm2mclient is the de-facto
# open-source LwM2M test client and is interop-tested against Leshan
# and most other implementations.
set -euo pipefail

NET=lwm2m-interop
SRV=iot-server
CLI=wakaama-cli
SNIFF=pcap-sniff
PCAP=log/L9/nfr-002-coap.pcap
PODMAN=/opt/homebrew/bin/podman

cleanup() {
    $PODMAN stop -i $SNIFF $CLI $SRV >/dev/null 2>&1 || true
    $PODMAN rm   -i $SNIFF $CLI $SRV >/dev/null 2>&1 || true
    $PODMAN network rm -f $NET       >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "[L9] (re)creating podman network $NET"
$PODMAN network rm -f $NET >/dev/null 2>&1 || true
$PODMAN network create $NET >/dev/null

echo "[L9] starting iot server (naushada/iot:latest, role=server)"
# Run with explicit DM port 5683 on `local=` so wakaama can register
# directly without going through Bootstrap. wakaama's default lifetime
# (300 s) keeps the run window short.
$PODMAN run -d --rm --name $SRV --network $NET \
    -v "$PWD/apps/config":/opt/app/config \
    naushada/iot:latest \
    /opt/app/lwm2m \
    "local=coap://0.0.0.0:5683" \
    "role=server" \
    "ep=urn:dev:client-1" \
    "config=/opt/app/config" >/dev/null

# Give the server ~3 s to bind sockets.
sleep 3
$PODMAN logs $SRV 2>&1 | tail -3 | sed 's/^/[iot-server] /'

echo "[L9] starting tcpdump sniffer attached to iot-server netns"
$PODMAN run -d --rm --name $SNIFF \
    --net=container:$SRV \
    --user=root \
    --cap-add=NET_RAW --cap-add=NET_ADMIN \
    -v "$PWD/log/L9":/cap \
    docker.io/nicolaka/netshoot:latest \
    tcpdump -i any -U -w /cap/nfr-002-coap.pcap "udp port 5683 or udp port 5684" \
    >/dev/null

sleep 3

echo "[L9] starting wakaama lwm2mclient → iot server"
$PODMAN run -d --rm --name $CLI --network $NET \
    docker.io/testingyourcode/wakaama-client \
    /root/lwm2mclient \
    -n urn:dev:wakaama-1 \
    -h "$SRV" \
    -p 5683 \
    -t 60 \
    -4 >/dev/null

echo "[L9] running for 45 s (Register + initial poll window)"
sleep 45

echo "[L9] sample of iot server log:"
$PODMAN logs $SRV 2>&1 | tail -12 | sed 's/^/[iot-server] /'
echo "[L9] sample of wakaama client log:"
$PODMAN logs $CLI 2>&1 | tail -8 | sed 's/^/[wakaama] /'

echo "[L9] tearing down (pcap closed)"
$PODMAN stop -i $SNIFF $CLI $SRV >/dev/null 2>&1 || true

echo "[L9] pcap → $PCAP"
ls -la $PCAP
