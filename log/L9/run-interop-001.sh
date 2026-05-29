#!/usr/bin/env bash
# NFR-INTEROP-001 runner: our binary (client) ↔ corfr/leshan (server)
# over plain CoAP. Captures a pcap to log/L9/nfr-001-coap.pcap.
#
# Usage:  bash log/L9/run-interop-001.sh
set -euo pipefail

NET=lwm2m-interop
SRV=leshan-iface
CLI=iot-client
SNIFF=pcap-sniff
PCAP=log/L9/nfr-001-coap.pcap
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

echo "[L9] starting Leshan server (corfr/leshan, amd64 via QEMU)"
# FUP-1: corfr/leshan:latest (built 2021) ships a Gson that uses
# unsafe reflection on java.util internals. JDK 17+'s module system
# blocks that, surfacing as InaccessibleObjectException in the
# EventServlet web-UI callback after every successful Register. The
# --add-opens flags below relax the module boundaries enough for Gson
# to JSON-serialize a Registration record. LwM2M protocol semantics
# are unaffected; this is purely a JVM/Gson compatibility patch.
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

# Give Leshan ~15 s to boot under QEMU.
sleep 18
$PODMAN logs $SRV 2>&1 | tail -3 | sed 's/^/[leshan] /'

echo "[L9] starting tcpdump sniffer attached to Leshan netns"
$PODMAN run -d --rm --name $SNIFF \
    --net=container:$SRV \
    --user=root \
    --cap-add=NET_RAW --cap-add=NET_ADMIN \
    -v "$PWD/log/L9":/cap \
    docker.io/nicolaka/netshoot:latest \
    tcpdump -i any -U -w /cap/nfr-001-coap.pcap "udp port 5683 or udp port 5684" \
    >/dev/null

# Let the sniffer initialise.
sleep 3

echo "[L9] starting iot client → Leshan"
$PODMAN run -d --rm --name $CLI --network $NET \
    -v "$PWD/apps/config":/opt/app/config \
    naushada/iot:latest \
    /opt/app/lwm2m \
    "local=coap://0.0.0.0:56830" \
    "bs=coap://$SRV:5683" \
    "role=client" \
    "ep=urn:dev:client-1" \
    "config=/opt/app/config" \
    >/dev/null

echo "[L9] running for 75 s (Register + initial tick window)"
sleep 75

echo "[L9] sample of iot client log:"
$PODMAN logs $CLI 2>&1 | tail -12 | sed 's/^/[iot] /'

echo "[L9] sample of Leshan server log:"
$PODMAN logs $SRV 2>&1 | tail -8  | sed 's/^/[leshan] /'

echo "[L9] tearing down (pcap closed)"
$PODMAN stop $SNIFF $CLI $SRV >/dev/null 2>&1 || true

echo "[L9] pcap → $PCAP"
ls -la $PCAP
