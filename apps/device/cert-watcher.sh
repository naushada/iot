#!/bin/sh
# cert-watcher — device-side certificate sidecar (xpmile pattern,
# iot-native reload).
#
# Watches a shared cert volume (/watch) for changes to the OpenVPN
# client-cert family and makes openvpn-client re-read the rotated certs.
# Same idea as xpmile's InnerTLS cert-watcher for wsdbagent, but adapted
# to iot's ds-coordinated architecture:
#
#   RELOAD_MODE=gate     (default) — flip services.openvpn.client.enable
#       false->true via ds-cli. The openvpn-client Supervisor's
#       ServiceGate reaps openvpn(8) on disable and respawns it on
#       enable; the respawn re-reads vpn.cert/key/ca paths → new certs.
#       Pure ds coordination: no container-engine socket needed, works
#       rootless. This is the recommended mode.
#
#   RELOAD_MODE=restart  — xpmile-faithful: POST /restart to the
#       container engine so the consumer container restarts. Needs the
#       engine API socket (PODMAN_SOCK) mounted; works on Linux device
#       hosts (rootful podman / docker), not mac rootless-in-container.
#
# Why poll (md5sum) not inotify: bind mounts across the podman machine
# VM don't reliably propagate inotify events; cert files are tiny.
#
# Cert SOURCE is external to this watcher (it only reacts to /watch
# changes). Per the chosen model, iot-cloudd mints a per-device VPN
# client cert and delivers it over the provisioning/ds plane; the
# delivery step writes ca.crt/client.crt/client.key into /watch, which
# trips this loop.
#
# Env:
#   WATCH_DIR        cert dir (default /watch)
#   RELOAD_MODE      gate | restart            (default gate)
#   DS_SOCK          ds-server socket          (default /run/iot/data_store.sock)
#   SVC_NAME         service gate to flip      (default openvpn.client)
#   POLL_SECONDS     poll interval             (default 5)
#   PODMAN_SOCK      engine socket (restart mode; default /run/podman/podman.sock)
#   TARGET_CONTAINER container to restart (restart mode; default iot-dev-ovpn)

set -eu
WATCH_DIR="${WATCH_DIR:-/watch}"
RELOAD_MODE="${RELOAD_MODE:-gate}"
DS_SOCK="${DS_SOCK:-/run/iot/data_store.sock}"
SVC_NAME="${SVC_NAME:-openvpn.client}"
POLL_SECONDS="${POLL_SECONDS:-5}"
SOCK="${PODMAN_SOCK:-/run/podman/podman.sock}"
TARGET_CONTAINER="${TARGET_CONTAINER:-iot-dev-ovpn}"

log() { echo "[cert-watcher] $*"; }

fingerprint() {
  find "$WATCH_DIR" -maxdepth 1 -type f \
    \( -name '*.crt' -o -name '*.key' -o -name '*.pem' \) \
    -exec md5sum {} + 2>/dev/null | sort | md5sum | cut -d' ' -f1
}

reload_gate() {
  S="--socket=$DS_SOCK"
  ds-cli $S set "services.$SVC_NAME.enable" false >/dev/null 2>&1 || { log "gate disable failed"; return 1; }
  sleep 1   # let the Supervisor reap openvpn(8) before re-enabling
  ds-cli $S set "services.$SVC_NAME.enable" true  >/dev/null 2>&1 || { log "gate enable failed";  return 1; }
  log "flipped services.$SVC_NAME.enable false->true (openvpn-client will respawn with new certs)"
}

api_base() {
  if curl -fsS -o /dev/null --unix-socket "$SOCK" "http://d/v4.0.0/libpod/_ping" 2>/dev/null; then
    echo "http://d/v4.0.0/libpod"; else echo "http://d/v1.41"; fi
}
reload_restart() {
  base=$(api_base)
  case "$base" in
    */libpod) url="$base/containers/$TARGET_CONTAINER/restart?timeout=5" ;;
    *)        url="$base/containers/$TARGET_CONTAINER/restart?t=5" ;;
  esac
  code=$(curl -fsS -o /dev/null -w '%{http_code}' -X POST --unix-socket "$SOCK" "$url" 2>/dev/null || echo 000)
  log "restart $TARGET_CONTAINER -> HTTP $code"
}

reload() {
  case "$RELOAD_MODE" in
    gate)    reload_gate ;;
    restart) reload_restart ;;
    *)       log "unknown RELOAD_MODE=$RELOAD_MODE"; return 1 ;;
  esac
}

log "mode=$RELOAD_MODE watch=$WATCH_DIR poll=${POLL_SECONDS}s"
[ "$RELOAD_MODE" = gate ] && log "reload via ds gate services.$SVC_NAME.enable (sock=$DS_SOCK)" \
                          || log "reload via engine restart of $TARGET_CONTAINER (sock=$SOCK)"

LAST=$(fingerprint || true)
log "initial cert fingerprint: ${LAST:-<none>}"
while true; do
  sleep "$POLL_SECONDS"
  NEW=$(fingerprint || true)
  if [ -n "$NEW" ] && [ "$NEW" != "$LAST" ]; then
    log "cert change detected ($LAST -> $NEW)"
    LAST="$NEW"
    reload || true
  fi
done
