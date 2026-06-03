SUMMARY = "IoT LwM2M stack package groups"
DESCRIPTION = "Tiered package groups for IoT LwM2M deployments. \
'core' installs the minimal device-side daemons. 'full' adds VPN, \
NAT/routing, and Wi-Fi management for gateway-class devices."
LICENSE = "MIT"

inherit packagegroup

# ── Core: minimal LwM2M device ────────────────────────────────────
# ds-server + lwm2m + ds-cli + config. Suitable for resource-constrained
# endpoints that only need CoAP device management.
RDEPENDS:${PN}-core = "\
    iot-ds-server \
    iot-lwm2m \
    iot-ds-cli \
    iot-config \
"
RRECOMMENDS:${PN}-core = "\
    kernel-module-tun \
"

# ── Full: gateway-class device ────────────────────────────────────
# Adds the HTTP REST API, OpenVPN, nftables NAT/routing, and Wi-Fi mgmt.
RDEPENDS:${PN}-full = "\
    ${PN}-core \
    iot-httpd \
    iot-openvpn-client \
    iot-net-router \
    iot-wifi-client \
    openvpn \
    nftables \
    iproute2 \
    wpa-supplicant \
    wireless-tools \
"

# ── Debug: includes test binaries ─────────────────────────────────
# For CI and developer images.
RDEPENDS:${PN}-debug = "\
    ${PN}-full \
"
# Note: test binaries (lwm2m_test, ds-tests, etc.) are not yet split
# into a separate iot-test package. Add them here when PACKAGECONFIG[gtest]
# packaging is completed.
