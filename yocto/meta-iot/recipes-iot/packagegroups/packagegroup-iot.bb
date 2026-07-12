SUMMARY = "IoT LwM2M stack package groups"
DESCRIPTION = "Tiered package groups for IoT LwM2M deployments. \
'core' installs the minimal device-side daemons. 'full' adds VPN, \
NAT/routing, and Wi-Fi management for gateway-class devices."
LICENSE = "MIT"

inherit packagegroup

# The packagegroup class does NOT auto-create tiered sub-packages — they
# must be declared here, otherwise nothing RPROVIDES packagegroup-iot-full
# and an image RDEPENDing on it fails to parse. The base ${PN} stays as an
# empty meta-package; -core/-full/-debug carry the RDEPENDS below.
PACKAGES = "\
    ${PN} \
    ${PN}-core \
    ${PN}-full \
    ${PN}-debug \
"

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
    iot-bcm2837-selftest \
    iot-sensord \
    iot-cellular \
    iot-containerd \
    iot-ddns \
    iot-smsctl \
    iot-pcap \
    openvpn \
    nftables \
    iproute2 \
    wpa-supplicant \
    crun \
"
# iot-pcap: ships the iot-pcap.service capture unit (DISABLED by default) and
# pulls tcpdump (+ libpcap) so field DTLS/CoAP capture is one `systemctl start`
# away without a reflash.
# crun: the OCI runtime iot-containerd shells out to (from meta-virtualization).
# Also pulled transitively via RDEPENDS:iot-containerd, listed here explicitly.
# Note: no wireless-tools (iwconfig/iwlist) — it was dropped from modern
# Yocto and the wifi-client daemon never used it; scanning goes through
# wpa_cli. The DHCP client comes from the recipe's
# RRECOMMENDS:iot-wifi-client = busybox-udhcpc.

# ── Debug: full stack + on-device debugging toolset ───────────────
# For CI and developer/field-debug images. Adds editors and the tools you
# reach for when ssh'd into a misbehaving RPi (process/syscall tracing,
# open-fd/socket inspection, packet capture, USB/I2C/MMC bus probing).
RDEPENDS:${PN}-debug = "\
    ${PN}-full \
    vim \
    nano \
    less \
    htop \
    procps \
    strace \
    ltrace \
    gdb \
    lsof \
    file \
    tree \
    curl \
    wget \
    socat \
    tcpdump \
    bind-utils \
    ethtool \
    usbutils \
    i2c-tools \
    mmc-utils \
"
# Note: test binaries (lwm2m_test, ds-tests, etc.) are not yet split
# into a separate iot-test package. Add them here when PACKAGECONFIG[gtest]
# packaging is completed.
