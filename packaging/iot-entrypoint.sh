#!/bin/sh
# Container entrypoint: dispatch on IOT_ROLE env var.
#
#   IOT_ROLE=ds      → run ds-server
#   IOT_ROLE=client  → run lwm2m role=client
#   IOT_ROLE=server  → run lwm2m role=server
#   IOT_ROLE=ovpn    → run openvpn-client (needs --cap-add=NET_ADMIN
#                       --device=/dev/net/tun on the podman run)
#   IOT_ROLE=net     → run net-router (needs --cap-add=NET_ADMIN +
#                       host's /proc/net visible; usually --network=host)
#   IOT_ROLE=wifi    → run wifi-client (needs --cap-add=NET_ADMIN
#                       --cap-add=NET_RAW + --network=host + host
#                       /dev/rfkill + a real wifi NIC. Container
#                       runs are dev-only; production is bare-metal
#                       systemd.)
#
# Per-role default args live in /etc/iot/lwm2m-*.env (sourced via
# . , so they end up as shell vars). Operators override by:
#   * mounting their own /etc/iot/ tree, OR
#   * passing `--env LWM2M_ARGS=...` on `podman run`, OR
#   * passing literal argv to the container (becomes "$@").
#
# Argv passed to the container goes through to the underlying binary,
# which lets you do:
#   podman run iot:l11 ds-cli set iot.endpoint '"urn:dev:c-7"'

set -e

# If the first arg is one of our binaries, forward verbatim — supports
# `podman run iot:l11 ds-cli get foo`.
case "${1:-}" in
    ds-cli|ds-server|lwm2m|openvpn-client|openvpn|net-router|nft|ip|wifi-client|wpa_supplicant|wpa_cli|udhcpc|dhclient)
        exec "$@"
        ;;
esac

case "${IOT_ROLE:-}" in
    ds)
        exec /usr/local/bin/ds-server \
            ds-socket=/run/iot/data_store.sock \
            ds-store=/var/lib/iot/data_store.lua \
            ds-schema-dir=/etc/iot/ds-schemas \
            "$@"
        ;;
    client)
        # shellcheck disable=SC1091
        [ -f /etc/iot/lwm2m-client.env ] && . /etc/iot/lwm2m-client.env
        : "${LWM2M_ARGS:?lwm2m-client.env missing LWM2M_ARGS}"
        # shellcheck disable=SC2086  # word-splitting is intentional
        exec /usr/local/bin/lwm2m $LWM2M_ARGS "$@"
        ;;
    server)
        # shellcheck disable=SC1091
        [ -f /etc/iot/lwm2m-server.env ] && . /etc/iot/lwm2m-server.env
        : "${LWM2M_ARGS:?lwm2m-server.env missing LWM2M_ARGS}"
        # shellcheck disable=SC2086
        exec /usr/local/bin/lwm2m $LWM2M_ARGS "$@"
        ;;
    ovpn)
        # shellcheck disable=SC1091
        [ -f /etc/iot/openvpn-client.env ] && . /etc/iot/openvpn-client.env
        : "${OPENVPN_CLIENT_ARGS:?openvpn-client.env missing OPENVPN_CLIENT_ARGS}"
        # shellcheck disable=SC2086
        exec /usr/local/bin/openvpn-client $OPENVPN_CLIENT_ARGS "$@"
        ;;
    net)
        # shellcheck disable=SC1091
        [ -f /etc/iot/net-router.env ] && . /etc/iot/net-router.env
        : "${NET_ROUTER_ARGS:?net-router.env missing NET_ROUTER_ARGS}"
        # shellcheck disable=SC2086
        exec /usr/local/bin/net-router $NET_ROUTER_ARGS "$@"
        ;;
    wifi)
        # shellcheck disable=SC1091
        [ -f /etc/iot/wifi-client.env ] && . /etc/iot/wifi-client.env
        : "${WIFI_CLIENT_ARGS:?wifi-client.env missing WIFI_CLIENT_ARGS}"
        # shellcheck disable=SC2086
        exec /usr/local/bin/wifi-client $WIFI_CLIENT_ARGS "$@"
        ;;
    "")
        echo "iot-entrypoint: set IOT_ROLE to one of: ds | client | server | ovpn | net | wifi" >&2
        echo "                or invoke a binary directly: ds-cli / ds-server / lwm2m / openvpn-client / openvpn / net-router / wifi-client / wpa_supplicant / wpa_cli / udhcpc / dhclient / nft / ip" >&2
        exit 64
        ;;
    *)
        echo "iot-entrypoint: unknown IOT_ROLE='$IOT_ROLE' (expected ds|client|server|ovpn|net|wifi)" >&2
        exit 64
        ;;
esac
