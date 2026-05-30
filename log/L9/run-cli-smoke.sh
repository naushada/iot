#!/usr/bin/env bash
# CLI smoke test for the polymorphic CommandRegistry. One client, one
# heredoc piped through script(1) for a real PTY, all 13 typed commands
# inside the same Readline loop. After a fixed wall window the client
# container is killed by name (NOT by local bash pid — that leaks
# containers). Server stays up for the whole run so all frames land
# in one pcap.
#
# Verifies each command emits its canonical CoAP request.
#
# Usage:  bash log/L9/run-cli-smoke.sh
set -euo pipefail

NET=lwm2m-smoke
SRV=smoke-server
CLI=smoke-client
SNIFF=smoke-sniff
PCAP=log/L9/cli-smoke.pcap
PODMAN=/opt/homebrew/bin/podman

EP=urn:dev:smoke-1

cleanup() {
    $PODMAN kill -s SIGKILL $SNIFF $CLI $SRV >/dev/null 2>&1 || true
    $PODMAN rm   -f         $SNIFF $CLI $SRV >/dev/null 2>&1 || true
    $PODMAN network rm -f   $NET             >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "[smoke] (re)creating podman network $NET"
$PODMAN network rm -f $NET >/dev/null 2>&1 || true
$PODMAN network create $NET >/dev/null

echo "[smoke] starting iot server on coap://0.0.0.0:5683"
$PODMAN run -d --rm --name $SRV --network $NET \
    -v "$PWD/apps/config":/opt/app/config \
    -v "$PWD/apps/build/lwm2m":/opt/app/lwm2m \
    naushada/iot:latest \
    /opt/app/lwm2m "local=coap://0.0.0.0:5683" "role=server" \
                   "ep=urn:dev:smoke-server" "config=/opt/app/config" >/dev/null
sleep 2

echo "[smoke] starting tcpdump sniffer"
$PODMAN run -d --rm --name $SNIFF --net=container:$SRV --user=root \
    --cap-add=NET_RAW -v "$PWD/log/L9":/cap \
    docker.io/nicolaka/netshoot:latest \
    tcpdump -i any -U -w /cap/cli-smoke.pcap "udp port 5683" >/dev/null
sleep 2

# 13 typed commands, 2s pacing, then `quit`. Total heredoc wall ≈ 28s.
# script(1) supplies a PTY so isatty() is true; the binary's Readline
# activates and reads from PTY slave. Per probe-readline.sh, this
# loop iterates correctly across many commands.
echo "[smoke] running 13-command client (≈30s window)"
$PODMAN run -d --name $CLI --network $NET \
    -v "$PWD/apps/config":/opt/app/config \
    -v "$PWD/apps/build/lwm2m":/opt/app/lwm2m \
    --entrypoint /bin/bash \
    naushada/iot:latest \
    -c "{
        sleep 2;  echo 'help';
        sleep 2;  echo 'register ep=$EP lt=60';
        sleep 2;  echo 'read path=/3/0/0';
        sleep 2;  echo 'write path=/3/0/15 value=Europe/Berlin';
        sleep 2;  echo 'execute path=/3/0/4';
        sleep 2;  echo 'observe path=/3/0/13';
        sleep 2;  echo 'delete path=/3/0';
        sleep 2;  echo 'bootstrap ep=$EP';
        sleep 2;  echo 'push ep=A12345 data=[{\"k\":\"v\"}]';
        sleep 2;  echo 'set ep=A12345 data={\"x\":1}';
        sleep 2;  echo 'get ep=A12345 data=[\"x\"]';
        sleep 2;  echo 'exec ep=A12345 data={\"action\":\"reboot\"}';
        sleep 2;  echo 'post uri=/push uri-query=ep=B67890';
        sleep 2;  echo 'device read=0';
        sleep 2;  echo 'server write=1 value=120';
        sleep 2;  echo 'firmware exec=2';
        sleep 2;  echo 'security read=0';
        sleep 2;  echo 'access-control read=3';
        sleep 2;  echo 'firmware read=7';
        sleep 2;  echo 'quit';
        sleep 1;
    } | script -qc '/opt/app/lwm2m local=coap://0.0.0.0:56830 bs=coap://$SRV:5683 role=client ep=$EP config=/opt/app/config' /dev/null" \
    >/dev/null

# Wait for natural exit (heredoc EOF → quit) or hard cap at 50s.
for i in $(seq 1 50); do
    if ! $PODMAN ps --format '{{.Names}}' | grep -q "^$CLI$"; then break; fi
    sleep 1
done
$PODMAN kill -s SIGKILL $CLI >/dev/null 2>&1 || true
$PODMAN logs $CLI >log/L9/cli-smoke-client.log 2>&1 || true

echo "[smoke] last 25 lines of client log:"
tail -25 log/L9/cli-smoke-client.log | sed 's/^/[client] /'

echo "[smoke] tearing down (pcap closed)"
$PODMAN kill $SNIFF $SRV >/dev/null 2>&1 || true
sleep 1

echo "[smoke] pcap → $PCAP"
ls -la $PCAP

echo "[smoke] wire frames:"
$PODMAN run --rm -v "$PWD/log/L9":/cap docker.io/nicolaka/netshoot:latest \
    tshark -r /cap/cli-smoke.pcap -nn 2>/dev/null | sed 's/^/  /'
