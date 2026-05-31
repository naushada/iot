#!/bin/sh
# L14 — compose helpers for the full-stack smoke.
#
# Sourced by log/L14/smoke.sh. Exports image tags, pod/container
# names, and host-side directory paths; provides compose_up and
# compose_down functions that wrap stock `podman` calls so the
# smoke doesn't need podman-compose / k8s tooling.
#
# Why hand-rolled instead of podman-compose.yml or `podman play kube`?
# - One file, one tool dependency (stock podman), no Python wrapper.
# - L12 prior art (log/L12/openvpn-smoke.sh) is also a flat shell
#   script — same debugging surface.
# - The fakes get bind-mounted in, so there's no Containerfile.l14
#   layer to maintain.
#
# Bring-up plumbing:
#   * One pod (POD_NAME) shares network + an emptyDir-style named
#     volume (VOL_RUN) mounted at /run/iot across the four iot
#     containers, so each daemon's DsBridge finds the ds-server
#     socket at /run/iot/data_store.sock.
#   * Fakes are bind-mounted read-only from FAKES_DIR; recorder dirs
#     are bind-mounted read-write from RECORDER_DIR so the smoke
#     can assert on what each daemon attempted.
#   * Leshan ports (5683/5684 UDP web + 8080 TCP API) are exposed
#     on the host via the pod's published ports.

set -eu

# ─────────────────────── config knobs ─────────────────────────
IOT_IMAGE="${IOT_IMAGE:-localhost/iot:l14}"
LESHAN_IMAGE="${LESHAN_IMAGE:-docker.io/dre/leshan-server-demo:latest}"
POD_NAME="${POD_NAME:-iot-l14}"
VOL_RUN="${VOL_RUN:-iot-l14-run}"

# Host-side dirs. The smoke uses a per-run mktemp -d so reruns don't
# stomp on each other; compose.sh just consumes whatever the caller
# (smoke.sh) exports.
: "${RUNDIR:?compose.sh expects RUNDIR exported by the caller}"
FAKES_DIR="${FAKES_DIR:-$(cd "$(dirname "$0")/fakes" && pwd)}"
RECORDER_DIR="${RECORDER_DIR:-$RUNDIR/recorder}"

# Container names — short slugs so `podman logs iot-ds` reads nice.
C_DS="iot-ds"
C_LWM2M="iot-lwm2m"
C_OVPN="iot-ovpn"
C_NETR="iot-netr"
C_LESHAN="iot-leshan"

# ─────────────────────── helpers ──────────────────────────────

# Build the iot image from the repo's Containerfile if absent. The
# build is cached, so reruns are fast; the explicit pull/build keeps
# the smoke self-contained.
ensure_iot_image() {
    if ! podman image exists "$IOT_IMAGE"; then
        echo "compose: building $IOT_IMAGE (one-time)..."
        podman build -f packaging/Containerfile -t "$IOT_IMAGE" .
    fi
}

# Pre-pull leshan so the pod creation doesn't stall on a slow
# registry round-trip mid-`podman run`.
ensure_leshan_image() {
    if ! podman image exists "$LESHAN_IMAGE"; then
        echo "compose: pulling $LESHAN_IMAGE (one-time)..."
        podman pull "$LESHAN_IMAGE"
    fi
}

# Idempotent: removes a pre-existing pod with the same name + a
# pre-existing named volume so re-runs start from a clean slate.
_reset_pod() {
    podman pod exists "$POD_NAME" && podman pod rm -f "$POD_NAME" >/dev/null
    podman volume exists "$VOL_RUN" && podman volume rm "$VOL_RUN" >/dev/null
    return 0
}

compose_up() {
    ensure_iot_image
    ensure_leshan_image
    _reset_pod
    mkdir -p "$RECORDER_DIR/ovpn" "$RECORDER_DIR/netr"

    podman volume create "$VOL_RUN" >/dev/null

    # Pod publishes leshan's UDP CoAP ports + its 8080 web API so the
    # smoke can curl /api/clients from the host.
    podman pod create \
        --name "$POD_NAME" \
        --publish 8080:8080/tcp \
        --publish 5683:5683/udp \
        --publish 5684:5684/udp \
        >/dev/null

    # ── ds-server ─────────────────────────────────────────────
    podman run -d --pod "$POD_NAME" --name "$C_DS" \
        -e IOT_ROLE=ds \
        -v "$VOL_RUN:/run/iot" \
        "$IOT_IMAGE" >/dev/null

    # ── leshan (CoAP/LwM2M demo server) ───────────────────────
    podman run -d --pod "$POD_NAME" --name "$C_LESHAN" \
        "$LESHAN_IMAGE" >/dev/null

    # ── lwm2m client ──────────────────────────────────────────
    # IOT_ROLE=client → entrypoint reads /etc/iot/lwm2m-client.env
    # for LWM2M_ARGS. Default endpoint comes from there; the smoke
    # can override via ds-cli set iot.endpoint after boot.
    podman run -d --pod "$POD_NAME" --name "$C_LWM2M" \
        -e IOT_ROLE=client \
        -v "$VOL_RUN:/run/iot" \
        "$IOT_IMAGE" >/dev/null

    # ── openvpn-client + fake openvpn ─────────────────────────
    # Override OPENVPN_CLIENT_ARGS so the daemon spawns the bind-
    # mounted fake instead of /usr/sbin/openvpn (which the runtime
    # image has but which would need cert material + a real peer).
    podman run -d --pod "$POD_NAME" --name "$C_OVPN" \
        -e IOT_ROLE=ovpn \
        -e OPENVPN_CLIENT_ARGS="--ds-sock=/run/iot/data_store.sock --openvpn=/fakes/fake-openvpn.sh" \
        -e FAKE_RECORDER_DIR=/recorder \
        -v "$VOL_RUN:/run/iot" \
        -v "$FAKES_DIR:/fakes:ro" \
        -v "$RECORDER_DIR/ovpn:/recorder:rw" \
        "$IOT_IMAGE" >/dev/null

    # ── net-router + fake nft ─────────────────────────────────
    # Same override pattern: --nft= points at the recorder script.
    # NET_ADMIN isn't strictly needed here because the fake never
    # touches the kernel, but we add it so this command line stays
    # identical to the production --nft=/usr/sbin/nft variant.
    podman run -d --pod "$POD_NAME" --name "$C_NETR" \
        --cap-add=NET_ADMIN \
        -e IOT_ROLE=net \
        -e NET_ROUTER_ARGS="--daemon --ds-sock=/run/iot/data_store.sock --nft=/fakes/fake-nft.sh --poll=2" \
        -e FAKE_RECORDER_DIR=/recorder \
        -v "$VOL_RUN:/run/iot" \
        -v "$FAKES_DIR:/fakes:ro" \
        -v "$RECORDER_DIR/netr:/recorder:rw" \
        "$IOT_IMAGE" >/dev/null
}

compose_down() {
    podman pod exists "$POD_NAME" && podman pod rm -f "$POD_NAME" >/dev/null
    podman volume exists "$VOL_RUN" && podman volume rm "$VOL_RUN" >/dev/null
    return 0
}

# Convenience: tail a container's logs through the pod boundary.
# Used by smoke.sh's failure path.
compose_logs() {
    name="$1"
    podman logs --tail 50 "$name" 2>&1 || true
}
