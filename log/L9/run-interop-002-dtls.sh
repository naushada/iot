#!/usr/bin/env bash
# NFR-INTEROP-002 DTLS variant: wakaama lwm2mclient_tinydtls ↔ our
# binary in server mode, DTLS/PSK. Captures pcap to
# log/L9/nfr-002-dtls.pcap.
#
# Uses the locally-built wakaama-dtls image (docker/Dockerfile.wakaama-dtls)
# which builds wakaama with DTLS=ON against eclipse/tinydtls master —
# Docker Hub doesn't ship a DTLS-enabled wakaama client image.
#
# Usage:  bash log/L9/run-interop-002-dtls.sh
set -euo pipefail

NET=lwm2m-interop
SRV=iot-server
CLI=wakaama-cli
SNIFF=pcap-sniff
PCAP=log/L9/nfr-002-dtls.pcap
PODMAN=/opt/homebrew/bin/podman

EP=urn:dev:wakaama-1
PSK_IDENTITY=97554878B284CE3B727D8DD06E87659A
PSK_HEX_KEY=3894beedaa7fe0eae6597dc350a59525

cleanup() {
    $PODMAN stop -i $SNIFF $CLI $SRV >/dev/null 2>&1 || true
    $PODMAN rm   -i $SNIFF $CLI $SRV >/dev/null 2>&1 || true
    $PODMAN network rm -f $NET       >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "[L9] (re)creating podman network $NET"
$PODMAN network rm -f $NET >/dev/null 2>&1 || true
$PODMAN network create $NET >/dev/null

echo "[L9] starting iot CoAPS server"
$PODMAN run -d --rm --name $SRV --network $NET \
    -v "$PWD/apps/config":/opt/app/config \
    naushada/iot:latest \
    /opt/app/lwm2m \
    "local=coaps://0.0.0.0:5684" \
    "role=server" \
    "ep=$EP" \
    "identity=$PSK_IDENTITY" \
    "secret=$PSK_HEX_KEY" \
    "config=/opt/app/config" >/dev/null

sleep 3
$PODMAN logs $SRV 2>&1 | tail -3 | sed 's/^/[iot-server] /'

echo "[L9] starting tcpdump sniffer attached to iot-server netns"
$PODMAN run -d --rm --name $SNIFF \
    --net=container:$SRV \
    --user=root \
    --cap-add=NET_RAW --cap-add=NET_ADMIN \
    -v "$PWD/log/L9":/cap \
    docker.io/nicolaka/netshoot:latest \
    tcpdump -i any -U -w /cap/nfr-002-dtls.pcap "udp port 5683 or udp port 5684" \
    >/dev/null

sleep 3

echo "[L9] starting wakaama DTLS client → iot CoAPS server"
$PODMAN run -d --rm --name $CLI --network $NET \
    --entrypoint /usr/local/bin/lwm2mclient \
    wakaama-dtls \
    -n "$EP" \
    -h "$SRV" \
    -p 5684 \
    -t 60 \
    -4 \
    -i "$PSK_IDENTITY" \
    -s "$PSK_HEX_KEY" >/dev/null

echo "[L9] running for 30 s (PSK handshake + Register)"
sleep 30

echo "[L9] iot server log:"
$PODMAN logs $SRV 2>&1 | tail -15 | sed 's/^/[iot-server] /'
echo "[L9] wakaama client log:"
$PODMAN logs $CLI 2>&1 | tail -10 | sed 's/^/[wakaama] /'

echo "[L9] tearing down (pcap closed)"
$PODMAN stop $SNIFF $CLI $SRV >/dev/null 2>&1 || true

echo "[L9] pcap → $PCAP"
ls -la $PCAP
