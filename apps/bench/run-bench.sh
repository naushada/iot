#!/usr/bin/env bash
#
# run-bench.sh — drive the cloud registration-plane load benchmark end to end.
#
# Brings up (or reuses) the apps/cloud compose stack, enables the zero-touch
# HKDF tier with a throwaway master + KEK, runs cloud_loadgen against lwm2m-bs
# from a container on the compose network, and samples cloud daemon CPU/mem
# during the soak.
#
# Usage:
#   apps/bench/run-bench.sh [count] [ramp] [soak]
#
# Env knobs (all optional):
#   COUNT=500 RAMP=50 SOAK=120        # devices, ramp/s, soak seconds
#   ENGINE=podman                     # or docker
#   NO_BUILD=1                        # skip image builds (reuse tags)
#   NO_UP=1                           # assume compose already running
#   CLOUD_DIR=apps/cloud
#
# Requires: podman (or docker) + the compose plugin. See
# apps/docs/tdd-cloud-load-benchmark.md.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CLOUD_DIR="${CLOUD_DIR:-$REPO/apps/cloud}"
ENGINE="${ENGINE:-podman}"
COMPOSE="$ENGINE compose"
# Benchmark against the locally-built cloud image (current code's HKDF resolver),
# not the published default. Override with CLOUD_IMAGE=.
export CLOUD_IMAGE="${CLOUD_IMAGE:-localhost/iot-cloud:local}"

COUNT="${1:-${COUNT:-200}}"
RAMP="${2:-${RAMP:-50}}"
SOAK="${3:-${SOAK:-60}}"
LIFETIME="${LIFETIME:-90}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-30}"

BUILDER_TAG="iot-cloud-builder:bench"
BENCH_TAG="iot-loadgen:latest"
DS="iot-ds-server"
SAMPLES_DIR="${SAMPLES_DIR:-$REPO/apps/bench/results}"
mkdir -p "$SAMPLES_DIR"
STAMP="$(date +%Y%m%d-%H%M%S)"

log() { printf '\033[1;36m[bench]\033[0m %s\n' "$*"; }

dscli() { $ENGINE exec "$DS" ds-cli --socket=/var/run/iot/data_store.sock "$@"; }

# ── 1. images ───────────────────────────────────────────────────────────────
if [ -z "${NO_BUILD:-}" ]; then
  log "building cloud builder image (one-time, ~minutes)…"
  $ENGINE build --target builder -t "$BUILDER_TAG" -f "$CLOUD_DIR/Dockerfile" "$REPO"
  log "building loadgen image…"
  $ENGINE build -t "$BENCH_TAG" -f "$REPO/apps/bench/Dockerfile" \
      --build-arg BUILDER="$BUILDER_TAG" "$REPO/apps/bench"
fi

# ── 2. compose up ────────────────────────────────────────────────────────────
# FRESH=1 wipes the compose volumes first — REQUIRED after a cloud image rebuild,
# because a stale named volume (cloud_iot-etc) shadows the image's ds-schemas and
# the new keys (e.g. cloud.bs.master.key) read as "not declared".
if [ -n "${FRESH:-}" ]; then
  log "FRESH=1 — tearing down + removing compose volumes…"
  ( cd "$CLOUD_DIR" && $COMPOSE down 2>/dev/null ) || true
  podman volume rm -f cloud_iot-etc cloud_iot-lib cloud_iot-run cloud_iot-tls \
      cloud_iot-vpn cloud_iot-ca-key cloud_iot-firmware \
      cloud_iot-telemetry-spool 2>/dev/null || true
fi

if [ -z "${NO_UP:-}" ]; then
  # Registration plane only: ds-server + the two LwM2M servers. Skip iot-cloudd
  # (needs /dev/net/tun for OpenVPN, off the registration path) and iot-httpd
  # via --no-deps so the two servers don't drag cloudd in.
  log "bringing up ds-server + lwm2m-bs + lwm2m-dm (image=$CLOUD_IMAGE)…"
  ( cd "$CLOUD_DIR" && $COMPOSE up -d ds-server )
  log "waiting for ds-server health…"
  for _ in $(seq 1 30); do
    if dscli get iot.version >/dev/null 2>&1; then break; fi
    sleep 2
  done
  ( cd "$CLOUD_DIR" && $COMPOSE up -d --no-deps lwm2m-bs lwm2m-dm )
fi

# Resolve the compose network the cloud containers share.
NET="$($ENGINE inspect "$DS" \
      --format '{{range $k,$v := .NetworkSettings.Networks}}{{$k}}{{end}}' \
      2>/dev/null | head -n1)"
[ -n "$NET" ] || { echo "could not resolve compose network for $DS" >&2; exit 1; }
log "compose network: $NET"

# ── 3. provision per-device credentials (manual tier, 128-bit keys) ──────────
# tinydtls caps PSKs at 16 B, so we use the manual provisioning tier (the
# HW-validated path), NOT zero-touch's 256-bit derivation (which overflows that
# buffer and never completes a handshake). MASTER is just a derivation seed so
# the loadgen and the provisioned rows agree byte-for-byte.
MASTER="$(openssl rand -hex 32)"
log "generating $COUNT device credentials…"
CREDS="$($ENGINE run --rm "$BENCH_TAG" \
          emit-creds=1 master="$MASTER" count="$COUNT" prefix=bench)"
[ "${CREDS:0:1}" = "[" ] || { echo "emit-creds produced no JSON" >&2; exit 1; }

log "writing cloud.endpoint.credentials + DM URI to data-store…"
# Stream the (possibly >128 KB) creds JSON via stdin — `ds-cli set <key> -` —
# to dodge the kernel's MAX_ARG_STRLEN single-argv cap.
printf '%s' "$CREDS" | $ENGINE exec -i "$DS" \
    ds-cli --socket=/var/run/iot/data_store.sock set cloud.endpoint.credentials -
dscli set cloud.dm.uri "coaps://iot-lwm2m-dm:5683"
dscli set cloud.dm.lifetime "$LIFETIME" || true
sleep 1

# ── 4. sample cloud resources during the run ─────────────────────────────────
STATS_CSV="$SAMPLES_DIR/stats-$STAMP.csv"
( echo "ts,container,cpu_pct,mem"; \
  for _ in $(seq 1 $(( (SOAK + COUNT / RAMP + 20) ))); do
    $ENGINE stats --no-stream --format \
      '{{.Name}},{{.CPUPerc}},{{.MemUsage}}' \
      iot-lwm2m-dm iot-lwm2m-bs iot-ds-server 2>/dev/null \
      | sed "s/^/$(date +%s),/"
    sleep 1
  done ) > "$STATS_CSV" 2>/dev/null &
STATS_PID=$!

# ── 5. run the load generator ────────────────────────────────────────────────
CSV="$SAMPLES_DIR/devices-$STAMP.csv"
log "running loadgen: count=$COUNT ramp=$RAMP/s soak=${SOAK}s → $NET"
set +e
$ENGINE run --rm --network "$NET" -v "$SAMPLES_DIR:/out" "$BENCH_TAG" \
    master="$MASTER" \
    bs-host=iot-lwm2m-bs bs-port=5684 \
    count="$COUNT" ramp="$RAMP" soak="$SOAK" \
    lifetime="$LIFETIME" boot-timeout="$BOOT_TIMEOUT" \
    csv="/out/devices-$STAMP.csv"
RC=$?
set -e

kill "$STATS_PID" 2>/dev/null || true
wait "$STATS_PID" 2>/dev/null || true

log "peak cloud CPU/mem during run (from $STATS_CSV):"
awk -F, 'NR>1 && $3!="" {gsub(/%/,"",$3); if($3+0>peak[$2]){peak[$2]=$3+0; mem[$2]=$4}}
         END{for(c in peak) printf "  %-16s cpu_peak=%.1f%%  mem@peak=%s\n", c, peak[c], mem[c]}' \
    "$STATS_CSV" || true

log "per-device CSV : $CSV"
log "resource CSV   : $STATS_CSV"
log "done (loadgen rc=$RC)"
exit "$RC"
