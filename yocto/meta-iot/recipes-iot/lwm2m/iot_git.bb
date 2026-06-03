SUMMARY = "IoT LwM2M 1.1.1 stack — device management daemons"
DESCRIPTION = "C++17 OMA LwM2M 1.1.1 device management stack built on ACE, \
CoAP, and DTLS (tinydtls). Delivers seven daemons as separate installable \
packages: ds-server (typed KV config plane), ds-cli (debug CLI), lwm2m \
(combined client/server), openvpn-client, net-router, wifi-client, \
iot-httpd (HTTP REST API fronting ds-server). \
Supports x86-64, ARM64, and ARMv7 targets via Yocto MACHINE selection."
HOMEPAGE = "https://github.com/naushada/iot"
LICENSE = "CLOSED"
SECTION = "net"

# ── Fetch ──────────────────────────────────────────────────────────
# gitsm:// fetches the repo with its two git submodules:
#   apps/3rdparty/json      → nlohmann/json (header-only)
#   apps/3rdparty/tinydtls  → legatoproject/tinydtls (headers only —
#                             the .a comes from the tinydtls recipe)
SRC_URI = "\
    gitsm://github.com/naushada/iot.git;protocol=https;branch=main \
    file://0001-cmake-use-yocto-sysroot-paths.patch \
    file://iot-ds.service \
    file://iot-lwm2m-client.service \
    file://iot-lwm2m-server.service \
    file://iot-openvpn-client.service \
    file://iot-net-router.service \
    file://iot-wifi-client.service \
    file://iot-httpd.service \
    file://iot.conf \
    file://lwm2m-client.env \
    file://lwm2m-server.env \
    file://openvpn-client.env \
    file://net-router.env \
    file://wifi-client.env \
    file://httpd.env \
"
SRCREV = "${AUTOREV}"

S = "${WORKDIR}/git"
B = "${WORKDIR}/build"

# ── Dependencies ────────────────────────────────────────────────────
# Core deps (always needed):
#   ace-tao            ACE reactor + OS abstraction (libACE.so)
#   tinydtls           DTLS static lib (libtinydtls.a)
#   lua                Lua 5.3 (required by data-store + lwm2m config)
#   openssl            SSL/crypto (DTLS, ACE_SSL, mongo TLS)
#   zlib               Compression (readline, mongo)
#   readline           Interactive CLI in lwm2m client mode
#   nlohmann-json      Header-only JSON (vendored, but DEPENDS for clarity)
DEPENDS = "\
    ace-tao \
    tinydtls \
    lua \
    openssl \
    zlib \
    readline \
    nlohmann-json \
"

# ── PACKAGECONFIG ──────────────────────────────────────────────────
# mongo:   Enables mongocxx/bsoncxx link (~15 MB in final image).
#          Required for the RegistryMirror feature (db_adapter.cpp).
#          Disable for resource-constrained embedded targets.
# gtest:   Builds unit-test binaries. Dev-only; OFF by default.
# systemd: Installs systemd unit files + env files + tmpfiles.d.
#          Auto-detected from DISTRO_FEATURES.
PACKAGECONFIG ??= "mongo \
                   ${@bb.utils.contains('DISTRO_FEATURES', 'systemd', 'systemd', '', d)} \
"

PACKAGECONFIG[mongo] = "\
    -DIOT_ENABLE_MONGO=ON \
      -DMONGOCXX_INCLUDE_DIR=${STAGING_INCDIR}/mongocxx/v_noabi \
      -DBSONCXX_INCLUDE_DIR=${STAGING_INCDIR}/bsoncxx/v_noabi, \
    -DIOT_ENABLE_MONGO=OFF, \
    mongo-cxx-driver mongo-c-driver, \
"

PACKAGECONFIG[gtest] = "\
    -DBUILD_DATA_STORE_TESTS=ON \
      -DBUILD_OPENVPN_CLIENT_TESTS=ON \
      -DBUILD_NET_ROUTER_TESTS=ON \
      -DBUILD_WIFI_CLIENT_TESTS=ON \
      -DBUILD_HTTP_SERVER_TESTS=ON, \
    -DBUILD_DATA_STORE_TESTS=OFF \
      -DBUILD_OPENVPN_CLIENT_TESTS=OFF \
      -DBUILD_NET_ROUTER_TESTS=OFF \
      -DBUILD_WIFI_CLIENT_TESTS=OFF \
      -DBUILD_HTTP_SERVER_TESTS=OFF, \
    gtest, \
"

PACKAGECONFIG[systemd] = "\
    -DIOT_PACKAGING_INSTALL=ON, \
    -DIOT_PACKAGING_INSTALL=OFF, \
    systemd, \
"

inherit ${@bb.utils.contains('PACKAGECONFIG', 'systemd', 'systemd', '', d)}
inherit cmake pkgconfig

# ── CMake configuration ─────────────────────────────────────────────
# OECMAKE_SOURCEPATH points cmake at the apps/CMakeLists.txt.
# Relative add_subdirectory() calls resolve against the full repo tree.
OECMAKE_SOURCEPATH = "${S}/apps"

# ACE_ROOT: set to sysroot /usr so include_directories(${ACE_ROOT}/include)
# resolves to ${STAGING_DIR_HOST}/usr/include (where ace-tao installs).
# tinydtls: the find_library() in our patch searches sysroot first.
EXTRA_OECMAKE = "\
    -DACE_ROOT=${STAGING_DIR_HOST}/usr \
    -DLUA_INCLUDE_DIR=${STAGING_INCDIR} \
    -DCMAKE_INSTALL_PREFIX=/usr \
"

# ── Sub-module cmake already uses GNUInstallDirs. Systemd + env files
# are overridden below from our WORKDIR copies so ExecStart uses /usr/bin
# rather than /usr/local/bin.
do_install() {
    cmake_do_install

    # ── systemd units (overwrite cmake-installed copies) ──────────
    if ${@bb.utils.contains('PACKAGECONFIG', 'systemd', 'true', 'false', d)}; then
        install -d ${D}${systemd_system_unitdir}
        install -m 0644 ${WORKDIR}/iot-ds.service            ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-lwm2m-client.service   ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-lwm2m-server.service   ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-openvpn-client.service ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-net-router.service     ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-wifi-client.service    ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-httpd.service          ${D}${systemd_system_unitdir}/

        # tmpfiles.d fallback
        install -d ${D}${nonarch_libdir}/tmpfiles.d
        install -m 0644 ${WORKDIR}/iot.conf ${D}${nonarch_libdir}/tmpfiles.d/

        # EnvironmentFile for each daemon
        install -d ${D}${sysconfdir}/iot
        install -m 0644 ${WORKDIR}/lwm2m-client.env   ${D}${sysconfdir}/iot/
        install -m 0644 ${WORKDIR}/lwm2m-server.env   ${D}${sysconfdir}/iot/
        install -m 0644 ${WORKDIR}/openvpn-client.env ${D}${sysconfdir}/iot/
        install -m 0644 ${WORKDIR}/net-router.env     ${D}${sysconfdir}/iot/
        install -m 0644 ${WORKDIR}/wifi-client.env    ${D}${sysconfdir}/iot/
        install -m 0644 ${WORKDIR}/httpd.env          ${D}${sysconfdir}/iot/
    fi
}

# ── Package split — one per daemon ─────────────────────────────────
# Images pull only what they need. A minimal LwM2M device needs only
# iot-ds-server + iot-lwm2m + iot-config. Gateway-class devices add
# iot-openvpn-client + iot-net-router + iot-wifi-client.

PACKAGE_BEFORE_PN = "\
    ${PN}-ds-server \
    ${PN}-ds-cli \
    ${PN}-lwm2m \
    ${PN}-openvpn-client \
    ${PN}-net-router \
    ${PN}-wifi-client \
    ${PN}-httpd \
    ${PN}-config \
"

# Main package: shared library + dev artefacts
FILES:${PN} = "\
    ${libdir}/libdatastore_client.a \
    ${includedir}/data_store \
"

# ds-server — the AF_UNIX config-plane daemon (required by all others)
FILES:${PN}-ds-server = "\
    ${bindir}/ds-server \
"
RDEPENDS:${PN}-ds-server = "\
    ace-tao \
    lua \
"

# ds-cli — operator debug/admin CLI
FILES:${PN}-ds-cli = "\
    ${bindir}/ds-cli \
"
RDEPENDS:${PN}-ds-cli = "ace-tao"

# lwm2m — combined LwM2M client + server binary (role selected at CLI)
FILES:${PN}-lwm2m = "\
    ${bindir}/lwm2m \
"
RDEPENDS:${PN}-lwm2m = "\
    ace-tao \
    lua \
    zlib \
    readline \
    openssl \
    tinydtls-staticdev \
    ${@bb.utils.contains('PACKAGECONFIG', 'mongo', \
        'mongo-cxx-driver-bsoncxx mongo-cxx-driver mongo-c-driver-bson mongo-c-driver', \
        '', d)} \
"
RRECOMMENDS:${PN}-lwm2m = "\
    ${PN}-ds-server \
    ${PN}-config \
"

# openvpn-client — OpenVPN tunnel supervisor
FILES:${PN}-openvpn-client = "\
    ${bindir}/openvpn-client \
"
RDEPENDS:${PN}-openvpn-client = "ace-tao openvpn"
RRECOMMENDS:${PN}-openvpn-client = "\
    ${PN}-ds-server \
    ${PN}-config \
"

# net-router — nftables + iproute2 network daemon
FILES:${PN}-net-router = "\
    ${bindir}/net-router \
"
RDEPENDS:${PN}-net-router = "\
    ace-tao \
    nftables \
    iproute2 \
"
RRECOMMENDS:${PN}-net-router = "\
    ${PN}-ds-server \
    ${PN}-config \
"

# wifi-client — wpa_supplicant + DHCP supervisor
FILES:${PN}-wifi-client = "\
    ${bindir}/wifi-client \
"
RDEPENDS:${PN}-wifi-client = "\
    ace-tao \
    wpa-supplicant \
"
RRECOMMENDS:${PN}-wifi-client = "\
    ${PN}-ds-server \
    ${PN}-config \
    busybox-udhcpc \
"

# httpd — HTTP REST API server fronting ds-server (L18)
FILES:${PN}-httpd = "\
    ${bindir}/iot-httpd \
"
RDEPENDS:${PN}-httpd = "ace-tao"
RRECOMMENDS:${PN}-httpd = "\
    ${PN}-ds-server \
    ${PN}-config \
"

# config — schema files, env files, config templates (shared substrate)
FILES:${PN}-config = "\
    ${sysconfdir}/iot \
"

# ── systemd ─────────────────────────────────────────────────────────
SYSTEMD_SERVICE:${PN}-ds-server = "iot-ds.service"
SYSTEMD_SERVICE:${PN}-lwm2m = "iot-lwm2m-client.service iot-lwm2m-server.service"
SYSTEMD_SERVICE:${PN}-openvpn-client = "iot-openvpn-client.service"
SYSTEMD_SERVICE:${PN}-net-router = "iot-net-router.service"
SYSTEMD_SERVICE:${PN}-wifi-client = "iot-wifi-client.service"
SYSTEMD_SERVICE:${PN}-httpd = "iot-httpd.service"

# Only ds-server auto-starts. Role units are enabled by the operator
# after writing the matching .env file — see DEPLOY.md.
SYSTEMD_AUTO_ENABLE:${PN}-ds-server = "enable"
SYSTEMD_AUTO_ENABLE:${PN}-lwm2m = "disable"
SYSTEMD_AUTO_ENABLE:${PN}-openvpn-client = "disable"
SYSTEMD_AUTO_ENABLE:${PN}-net-router = "disable"
SYSTEMD_AUTO_ENABLE:${PN}-wifi-client = "disable"
SYSTEMD_AUTO_ENABLE:${PN}-httpd = "disable"

# ── Sanity checks ──────────────────────────────────────────────────
# Inhibit the "binary already stripped" QA warning — we build with -g
# and cmake may strip at install. Let Yocto's packaging handle it.
INSANE_SKIP:${PN}-lwm2m = "already-stripped"
INSANE_SKIP:${PN}-ds-server = "already-stripped"
INSANE_SKIP:${PN}-ds-cli = "already-stripped"
INSANE_SKIP:${PN}-openvpn-client = "already-stripped"
INSANE_SKIP:${PN}-net-router = "already-stripped"
INSANE_SKIP:${PN}-wifi-client = "already-stripped"
INSANE_SKIP:${PN}-httpd = "already-stripped"
