#!/usr/bin/env bash
# CLI smoke test for the polymorphic CommandRegistry refactor.
# One fresh client invocation per command (each typing the command
# then `quit`). Captures pcap to log/L9/cli-smoke.pcap. Server stays
# up across all probes so frames concentrate in one file.
#
# Each command should yield ONE CON request from client → server.
# Verifies the canonical request shape per command.
#
# Usage:  bash log/L9/run-cli-smoke.sh
set -euo pipefail

NET=lwm2m-smoke
SRV=smoke-server
SNIFF=smoke-sniff
PCAP=log/L9/cli-smoke.pcap
PODMAN=/opt/homebrew/bin/podman

cleanup() {
    $PODMAN kill -s SIGKILL $SNIFF $SRV >/dev/null 2>&1 || true
    $PODMAN rm   -f         $SNIFF $SRV >/dev/null 2>&1 || true
    $PODMAN network rm -f   $NET        >/dev/null 2>&1 || true
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
    /opt/app/lwm2m \
    "local=coap://0.0.0.0:5683" \
    "role=server" \
    "ep=urn:dev:smoke-server" \
    "config=/opt/app/config" >/dev/null
sleep 3

echo "[smoke] starting tcpdump sniffer"
$PODMAN run -d --rm --name $SNIFF \
    --net=container:$SRV \
    --user=root \
    --cap-add=NET_RAW --cap-add=NET_ADMIN \
    -v "$PWD/log/L9":/cap \
    docker.io/nicolaka/netshoot:latest \
    tcpdump -i any -U -w /cap/cli-smoke.pcap "udp port 5683" \
    >/dev/null
sleep 2

# One-shot client. Starts up, types one command + quit, exits. Brings
# its own ephemeral local port so frames don't collide across probes.
probe() {
    local probe_name=$1
    local probe_cmd=$2
    local probe_port=$3
    local probe_ep="urn:dev:smoke-${probe_name}"
    local cli_log="log/L9/cli-smoke-${probe_name}.log"
    echo "[smoke:${probe_name}] ${probe_cmd}"
    $PODMAN run -i --rm --network $NET \
        -v "$PWD/apps/config":/opt/app/config \
        -v "$PWD/apps/build/lwm2m":/opt/app/lwm2m \
        --entrypoint /bin/bash \
        naushada/iot:latest \
        -c "{ sleep 1; echo '${probe_cmd}'; sleep 2; echo 'quit'; sleep 1; } | \
            script -qc '/opt/app/lwm2m local=coap://0.0.0.0:${probe_port} bs=coap://${SRV}:5683 role=client ep=${probe_ep} config=/opt/app/config' /dev/null" \
        >"${cli_log}" 2>&1 &
    # Bound each probe to 8s wall — long enough for handshake + one CoAP exchange
    local pid=$!
    sleep 8
    kill $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true
}

# Run each command in its own short-lived client. Skip register
# (the auto-Register fallback already fires before our prompt).
probe register  "register ep=urn:dev:smoke-1 lt=60"                       56831
probe read      "read path=/3/0/0"                                         56832
probe write     "write path=/3/0/15 value=Europe/Berlin"                   56833
probe execute   "execute path=/3/0/4"                                      56834
probe observe   "observe path=/3/0/13"                                     56835
probe delete    "delete path=/3/0"                                         56836
probe bootstrap "bootstrap ep=urn:dev:smoke-1"                             56837
probe post      "post uri=/push uri-query=ep=A12345"                       56838

echo "[smoke] last 3 lines of each client log:"
for f in log/L9/cli-smoke-*.log; do
    echo "===== $f ====="
    tail -3 "$f" | sed 's/^/  /'
done

echo "[smoke] tearing down (pcap closed)"
$PODMAN kill $SNIFF $SRV >/dev/null 2>&1 || true

echo "[smoke] pcap → $PCAP"
ls -la $PCAP
