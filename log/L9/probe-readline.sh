#!/usr/bin/env bash
# Tight repro for the post-first-command hang. Server up, sniff,
# then ONE client with TWO commands + quit, separated by 3s. The
# [rl] debug prints in readline.cpp tell us exactly which step of
# the loop wedges.
set -euo pipefail

NET=rl-probe
SRV=rl-server
SNIFF=rl-sniff
CLI=rl-client
PODMAN=/opt/homebrew/bin/podman

cleanup() {
    $PODMAN kill -s SIGKILL $SNIFF $CLI $SRV >/dev/null 2>&1 || true
    $PODMAN rm   -f         $SNIFF $CLI $SRV >/dev/null 2>&1 || true
    $PODMAN network rm -f   $NET             >/dev/null 2>&1 || true
}
trap cleanup EXIT

$PODMAN network rm -f $NET >/dev/null 2>&1 || true
$PODMAN network create $NET >/dev/null

$PODMAN run -d --rm --name $SRV --network $NET \
    -v "$PWD/apps/config":/opt/app/config \
    -v "$PWD/apps/build/lwm2m":/opt/app/lwm2m \
    naushada/iot:latest \
    /opt/app/lwm2m local=coap://0.0.0.0:5683 role=server ep=urn:dev:rl-server config=/opt/app/config >/dev/null
sleep 2

$PODMAN run -d --rm --name $SNIFF --net=container:$SRV --user=root \
    --cap-add=NET_RAW -v "$PWD/log/L9":/cap \
    docker.io/nicolaka/netshoot:latest \
    tcpdump -i any -U -w /cap/probe-readline.pcap "udp port 5683" >/dev/null
sleep 2

# 2 commands + quit. If readline loop is healthy, expect 2 register frames in pcap.
$PODMAN run -i --rm --name $CLI --network $NET \
    -v "$PWD/apps/config":/opt/app/config \
    -v "$PWD/apps/build/lwm2m":/opt/app/lwm2m \
    --entrypoint /bin/bash \
    naushada/iot:latest \
    -c "{ sleep 2; echo 'register ep=urn:dev:probe-a lt=60'; sleep 4; echo 'register ep=urn:dev:probe-b lt=60'; sleep 4; echo 'quit'; sleep 1; } | \
        script -qc '/opt/app/lwm2m local=coap://0.0.0.0:56830 bs=coap://$SRV:5683 role=client ep=urn:dev:rl-client config=/opt/app/config' /dev/null" \
    >log/L9/probe-readline.client.log 2>&1 &
CLI_PID=$!

# Bound the client to 18s (2 + 4 + 4 + 1 + slack); then SIGKILL the container by name.
sleep 18
$PODMAN kill -s SIGKILL $CLI >/dev/null 2>&1 || true
wait $CLI_PID 2>/dev/null || true

echo "===== client stderr (debug rl iters) ====="
grep "\[rl\]" log/L9/probe-readline.client.log || echo "(no [rl] lines captured)"
echo "===== pcap frames ====="
$PODMAN run --rm -v "$PWD/log/L9":/cap docker.io/nicolaka/netshoot:latest \
    tshark -r /cap/probe-readline.pcap -nn 2>/dev/null || true

$PODMAN kill $SNIFF $SRV >/dev/null 2>&1 || true
