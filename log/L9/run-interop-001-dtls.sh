#!/usr/bin/env bash
# NFR-INTEROP-001 (DTLS variant): our binary (client, CoAPS) ↔
# corfr/leshan (server, CoAPS). Captures pcap to
# log/L9/nfr-001-dtls.pcap and verifies tinydtls completes the
# PSK handshake against Leshan's DTLS endpoint on 5684.
#
# Uses the PSK identity/secret pair already pinned by `cmd/command.txt`
# and `apps/src/dtls_adapter.cpp`:
#   identity = 97554878B284CE3B727D8DD06E87659A   (32 bytes on the wire)
#   secret   = 3894beedaa7fe0eae6597dc350a59525   (16 bytes binary, hex on cli)
#
# Usage:  bash log/L9/run-interop-001-dtls.sh
set -euo pipefail

NET=lwm2m-interop
SRV=leshan-iface
CLI=iot-client
SNIFF=pcap-sniff
PCAP=log/L9/nfr-001-dtls.pcap
PODMAN=/opt/homebrew/bin/podman

EP=urn:dev:client-1
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

echo "[L9] starting Leshan server (corfr/leshan, amd64 via QEMU) with --add-opens"
$PODMAN run -d --rm --name $SRV --network $NET \
    --entrypoint sh \
    -p 8080:8080 -p 5683:5683/udp -p 5684:5684/udp \
    docker.io/corfr/leshan:latest \
    -c 'cd /opt/leshan && exec java \
        --add-opens java.base/java.util=ALL-UNNAMED \
        --add-opens java.base/java.lang=ALL-UNNAMED \
        --add-opens java.base/java.util.regex=ALL-UNNAMED \
        --add-opens java.base/java.text=ALL-UNNAMED \
        -jar ./leshan-server-demo.jar' >/dev/null

sleep 20
$PODMAN logs $SRV 2>&1 | tail -3 | sed 's/^/[leshan] /'

echo "[L9] registering PSK credential for $EP on Leshan via REST"
# Leshan demo's /api/security endpoint accepts a Security entry.
# Identity is the ASCII string (sent verbatim on the wire); key is the
# hex of the binary 16-byte PSK.
$PODMAN run --rm --network $NET \
    docker.io/nicolaka/netshoot:latest \
    curl -sS -X PUT "http://$SRV:8080/api/security/clients/" \
        -H "Content-Type: application/json" \
        -d "{\"endpoint\":\"$EP\",\"psk\":{\"identity\":\"$PSK_IDENTITY\",\"key\":\"$PSK_HEX_KEY\"}}" \
        -o /dev/null -w "[leshan-psk] http=%{http_code}\n" || true
sleep 1

echo "[L9] starting tcpdump sniffer attached to Leshan netns"
$PODMAN run -d --rm --name $SNIFF \
    --net=container:$SRV \
    --user=root \
    --cap-add=NET_RAW --cap-add=NET_ADMIN \
    -v "$PWD/log/L9":/cap \
    docker.io/nicolaka/netshoot:latest \
    tcpdump -i any -U -w /cap/nfr-001-dtls.pcap "udp port 5683 or udp port 5684" \
    >/dev/null

sleep 3

echo "[L9] starting iot CoAPS client → Leshan (PSK handshake)"
$PODMAN run -d --rm --name $CLI --network $NET \
    -v "$PWD/apps/config":/opt/app/config \
    naushada/iot:latest \
    /opt/app/lwm2m \
    "local=coaps://0.0.0.0:56830" \
    "bs=coaps://$SRV:5684" \
    "role=client" \
    "identity=$PSK_IDENTITY" \
    "secret=$PSK_HEX_KEY" \
    "ep=$EP" \
    "config=/opt/app/config" >/dev/null

echo "[L9] running for 30 s (DTLS handshake + Register window)"
sleep 30

echo "[L9] iot client log (last 25 lines):"
$PODMAN logs $CLI 2>&1 | tail -25 | sed 's/^/[iot] /'
echo "[L9] Leshan log tail:"
$PODMAN logs $SRV 2>&1 | tail -8 | sed 's/^/[leshan] /'

echo "[L9] tearing down (pcap closed)"
$PODMAN stop $SNIFF $CLI $SRV >/dev/null 2>&1 || true

echo "[L9] pcap → $PCAP"
ls -la $PCAP
