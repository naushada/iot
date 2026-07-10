#!/bin/sh
### BEGIN INIT INFO
# Provides:          iot-ecm-nat
# Required-Start:    $network
# Default-Start:     3 4 5
# Default-Stop:      0 1 6
# Short-Description: NAT the WP7702 cellular bearer out to the ECM host
### END INIT INFO
#
# Runs ON THE WP7702 MODULE (swi-mdm9x28-wp), not on the Raspberry Pi.
#
# Why this exists
# ---------------
# The WP7702 is a Linux module that owns the cellular data session on its own
# internal rmnet_data0. An external USB host (our RPi3B) CANNOT open a data call
# on wwan0 — the firmware refuses it (AT$QCRMCALL -> NO CARRIER, AT!SCACT ->
# "no network service"). The supported host path is the ECM link:
#
#     RPi eth1 (192.168.2.3) <--USB ECM--> module ecm0 (192.168.2.2)
#                                              | NAT
#                                              v
#                                          rmnet_data0 --> carrier
#
# This script installs the forwarding + masquerade rules that make the module a
# NAT router for the Pi. Without them the Pi can reach 192.168.2.2 and nothing
# beyond it.
#
# The module's FORWARD policy is DROP, so BOTH directions are required: the
# forward rule for new outbound flows, and a conntrack rule for return traffic.
# Omitting the return rule yields a NAT that silently drops every reply.
#
# Installation (the module's /etc is a writable, flash-backed overlay:
# upperdir=/mnt/flash/ufs/etc — so this survives a module reboot, but NOT a
# module firmware update; re-provision it after any WP firmware upgrade):
#
#     scp packaging/mangoh/iot-ecm-nat.sh root@192.168.2.2:/etc/init.d/iot-ecm-nat
#     ssh root@192.168.2.2 'chmod +x /etc/init.d/iot-ecm-nat &&
#                           ln -sf ../init.d/iot-ecm-nat /etc/rc5.d/S99iot-ecm-nat &&
#                           /etc/init.d/iot-ecm-nat start'
#
# (ssh needs -o HostKeyAlgorithms=+ssh-rsa; the module offers only ssh-rsa.)
#
# On the Pi side, the default route via 192.168.2.2 is installed by
# 05-iot-cellular-ecm.network. See apps/docs/hw-bringup-wp7702-cellular-wan.md.

PATH=/sbin:/usr/sbin:/bin:/usr/bin
WAN_IF=rmnet_data0     # the module's internal cellular bearer
LAN_IF=ecm0            # the USB ECM link to the Pi

start() {
    echo 1 > /proc/sys/net/ipv4/ip_forward

    # -C tests for an existing identical rule, so start is idempotent.
    iptables -t nat -C POSTROUTING -o "$WAN_IF" -j MASQUERADE 2>/dev/null || \
        iptables -t nat -A POSTROUTING -o "$WAN_IF" -j MASQUERADE

    iptables -C FORWARD -i "$LAN_IF" -o "$WAN_IF" -j ACCEPT 2>/dev/null || \
        iptables -A FORWARD -i "$LAN_IF" -o "$WAN_IF" -j ACCEPT

    # Return path. The module's FORWARD policy is DROP; without this every
    # reply packet is dropped and the NAT appears to "work" but carries nothing.
    iptables -C FORWARD -i "$WAN_IF" -o "$LAN_IF" \
        -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || \
        iptables -A FORWARD -i "$WAN_IF" -o "$LAN_IF" \
            -m state --state RELATED,ESTABLISHED -j ACCEPT

    echo "iot-ecm-nat: $LAN_IF -> $WAN_IF masquerade active"
}

stop() {
    iptables -t nat -D POSTROUTING -o "$WAN_IF" -j MASQUERADE 2>/dev/null
    iptables -D FORWARD -i "$LAN_IF" -o "$WAN_IF" -j ACCEPT 2>/dev/null
    iptables -D FORWARD -i "$WAN_IF" -o "$LAN_IF" \
        -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null
    echo 0 > /proc/sys/net/ipv4/ip_forward
    echo "iot-ecm-nat: stopped"
}

status() {
    echo "ip_forward=$(cat /proc/sys/net/ipv4/ip_forward)"
    iptables -t nat -S POSTROUTING | grep MASQUERADE || echo "(no masquerade rule)"
    iptables -S FORWARD | grep -E "$LAN_IF|$WAN_IF" || echo "(no forward rules)"
}

case "$1" in
    start)   start ;;
    stop)    stop ;;
    restart) stop; start ;;
    status)  status ;;
    *)       echo "Usage: $0 {start|stop|restart|status}" >&2; exit 1 ;;
esac
