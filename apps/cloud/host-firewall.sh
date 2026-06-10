#!/usr/bin/env bash
#
# host-firewall.sh — open the host firewall for the IoT Cloud server.
#
# Run once on the cloud host, as root:
#
#     sudo ./host-firewall.sh
#
# Idempotent: re-running never adds duplicate rules. Rules are persisted so
# they survive a reboot (netfilter-persistent on Debian/Ubuntu, else saved
# to /etc/iptables/rules.v4).
#
# IMPORTANT — two things this does NOT replace:
#  1. Your cloud-provider firewall (Contabo Cloud Firewall, AWS security
#     group, etc.) must ALSO allow these ports inbound. On a VPS the
#     provider firewall is usually what blocks you, not host iptables.
#  2. The cloud runs in Docker, which inserts its own iptables rules for
#     published ports and forwards that traffic via the FORWARD chain
#     (it does not hit INPUT). The INPUT allows below matter for the host
#     itself (e.g. SSH) and for hosts with a default-DROP INPUT policy;
#     for the Docker-published ports they are harmless belt-and-suspenders.
#
set -euo pipefail

# ── Ports to open ── one entry per line: "proto port description".
# Edit this list to match your deployment.
RULES=(
  "tcp  22   SSH — keep this or you lose remote access"
  "tcp  80   Cloud UI + REST API (http, or https-redirect)"
  "tcp  443  Cloud UI + REST API (https, when HTTPS=1)"
  "udp  5684 LwM2M Bootstrap server"
  "udp  5683 LwM2M Device Management server"
  "tcp  1194 OpenVPN tunnel server (cloud.vpn.proto=tcp-server)"
)

require_root() {
  if [ "$(id -u)" -ne 0 ]; then
    echo "ERROR: run as root — try: sudo $0" >&2
    exit 1
  fi
}

open_port() {
  local proto="$1" port="$2"; shift 2
  local desc="$*"
  if iptables -C INPUT -p "$proto" --dport "$port" -j ACCEPT 2>/dev/null; then
    printf '  = %-3s %-5s already allowed  (%s)\n' "$proto" "$port" "$desc"
  else
    iptables -I INPUT -p "$proto" --dport "$port" -j ACCEPT
    printf '  + %-3s %-5s opened          (%s)\n' "$proto" "$port" "$desc"
  fi
}

persist() {
  if command -v netfilter-persistent >/dev/null 2>&1; then
    netfilter-persistent save && echo "Persisted via netfilter-persistent (survives reboot)."
  elif command -v iptables-save >/dev/null 2>&1 && [ -d /etc/iptables ]; then
    iptables-save > /etc/iptables/rules.v4 && echo "Persisted to /etc/iptables/rules.v4."
  else
    cat >&2 <<'EOF'
NOTE: rules are active now but NOT persistent across reboot.
To persist on Debian/Ubuntu:
    apt-get install -y iptables-persistent
    netfilter-persistent save
EOF
  fi
}

require_root
echo "Opening IoT Cloud host firewall ports..."
for rule in "${RULES[@]}"; do
  # shellcheck disable=SC2086  # intentional word-split of "proto port desc"
  open_port $rule
done
persist
echo
echo "Reminder: the cloud-provider firewall must ALSO allow these ports inbound."
