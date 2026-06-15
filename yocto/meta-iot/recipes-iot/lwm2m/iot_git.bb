SUMMARY = "IoT LwM2M 1.1.1 stack — device management daemons"
DESCRIPTION = "C++17 OMA LwM2M 1.1.1 device management stack built on ACE, \
CoAP, and DTLS (tinydtls). Delivers seven daemons as separate installable \
packages: ds-server (typed KV config plane), ds-cli (debug CLI), lwm2m \
(combined client/server), openvpn-client, net-router, wifi-client, \
iot-httpd (HTTP REST API fronting ds-server). \
Supports x86-64, ARM64, and ARMv7 targets via Yocto MACHINE selection."
HOMEPAGE = "https://github.com/naushada/iot"
LICENSE = "MIT"
# Repo-root LICENSE file (fetched into ${S}). Update the md5 if the LICENSE text
# (e.g. the copyright year) changes.
LIC_FILES_CHKSUM = "file://LICENSE;md5=5b1ad2e89d6319fe2af63290833075ef"
SECTION = "net"

# ── Fetch ──────────────────────────────────────────────────────────
# Plain git:// (NOT gitsm://): apps/3rdparty/{json,tinydtls} are committed
# into the repo as plain trees, not live submodule gitlinks (.gitmodules is
# stale). gitsm mistakes the tree hash for a submodule commit and fails to
# fetch it from nlohmann/json. git:// fetches the committed files directly:
#   apps/3rdparty/json/single_include  → nlohmann/json (used by the build)
#   tinydtls headers + .a come from the separate tinydtls recipe (sysroot).
# Branch/ref to build. Defaults to `main` (rolling dev image). For a frozen
# release image, override (here on a release branch, or in local.conf):
#   IOT_BRANCH = "release/v1.1.0"
IOT_BRANCH ?= "main"
SRC_URI = "\
    git://github.com/naushada/iot.git;protocol=https;branch=${IOT_BRANCH} \
    file://0001-cmake-use-yocto-sysroot-paths.patch \
    file://iot-ds.service \
    file://iot-lwm2m-client.service \
    file://iot-lwm2m-server.service \
    file://iot-openvpn-client.service \
    file://iot-vpn-cert.path \
    file://iot-vpn-cert.service \
    file://iot-tun.conf \
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
    file://iot-ota-stage \
    file://iot-swupdate \
    file://iot-swupdate.path \
    file://iot-swupdate.service \
    file://iot-ota-confirm \
    file://iot-ota-confirm.service \
    file://migrations/README.md \
    file://migrations/0000-template.sh.example \
    file://gen_wifi_default.py \
"

# Optional, gitignored WiFi credential seed. When an integrator drops
# files/wifi_credentials.lua into this layer, bake it into the wifi.networks
# schema default at build time (see do_install:append + apps/docs/
# tdd-wifi-credentials-seed.md). Adding it to SRC_URI only when present keeps
# it optional AND part of the recipe signature, so changing the credentials
# triggers a rebuild. Absent => no-op, the committed "changeme" placeholder
# stands.
SRC_URI += "${@'file://wifi_credentials.lua' if os.path.exists(d.getVar('THISDIR') + '/files/wifi_credentials.lua') else ''}"

SRCREV = "${AUTOREV}"

# Package version. Keep the leading semver in sync with the repo-root /VERSION
# file (the single source of truth that iot-httpd also bakes into iot.version).
# PV can't read the fetched VERSION at parse time (source isn't unpacked yet),
# so it's set here manually. The +git${SRCPV} suffix gives opkg per-commit
# upgrade ordering (AUTOINC+sha) for the _git recipe.
PV = "1.1.0+git${SRCPV}"

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
    nodejs-native \
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
    -DIOT_ENABLE_MONGO=ON, \
    -DIOT_ENABLE_MONGO=OFF, \
    mongo-cxx-driver mongo-c-driver, \
"

PACKAGECONFIG[gtest] = "\
    -DBUILD_APPS_TESTS=ON \
      -DBUILD_DATA_STORE_TESTS=ON \
      -DBUILD_OPENVPN_CLIENT_TESTS=ON \
      -DBUILD_NET_ROUTER_TESTS=ON \
      -DBUILD_WIFI_CLIENT_TESTS=ON \
      -DBUILD_HTTP_SERVER_TESTS=ON, \
    -DBUILD_APPS_TESTS=OFF \
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
inherit cmake pkgconfig useradd

# ── PSK provisioning accounts (apps/docs/tdd-psk-provisioning.md) ────
# The LwM2M client runs as the static `engineer` account so the
# data-store read_acl/write_acl (gid:engineer) can match it — only this
# client may read the write-only PSK keys. The shared `iot` group lets
# `engineer` reach ds-server's 0660 socket (ds-server chgrp's the socket
# to `iot`). The lwm2m package owns these accounts.
USERADD_PACKAGES = "${PN}-lwm2m"
GROUPADD_PARAM:${PN}-lwm2m = "--system engineer; --system iot"
USERADD_PARAM:${PN}-lwm2m = "--system --no-create-home \
    --shell /usr/sbin/nologin --gid engineer --groups iot \
    --comment 'IoT LwM2M client' engineer"

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
    -DTINYDTLS_LIBRARY=${STAGING_LIBDIR}/libtinydtls.a \
    -DTINYDTLS_INCLUDE_DIR=${STAGING_INCDIR}/tinydtls \
    -DMONGOCXX_INCLUDE_DIR=${STAGING_INCDIR}/mongocxx/v_noabi \
    -DBSONCXX_INCLUDE_DIR=${STAGING_INCDIR}/bsoncxx/v_noabi \
    -DACE_LIBRARY=${STAGING_LIBDIR}/libACE.so \
    -DBSONCXX_LIBRARY=${STAGING_LIBDIR}/libbsoncxx.so \
    -DMONGOCXX_LIBRARY=${STAGING_LIBDIR}/libmongocxx.so \
    -DCMAKE_INSTALL_PREFIX=/usr \
"

# ── Device UI (Angular SPA) ─────────────────────────────────────────
# modules/http-server/CMakeLists.txt stages iot-ui/dist/iot-ui →
# /usr/share/iot/www, but ONLY if the dist exists at *configure* time.
# Yocto clones the repo without a prebuilt dist (iot-ui/dist is .gitignore'd),
# so build the SPA here with nodejs-native BEFORE do_configure runs.
#
# `npm` needs network — granted via the do_build_ui[network] flag below.
# For a fully offline/reproducible build, replace this with the `npm`
# bbclass + an npmsw:// SRC_URI (fetched at do_fetch). nodejs-native must
# satisfy the Angular CLI engine requirement (Angular 14 → Node 18/20).
do_build_ui() {
    cd ${S}/iot-ui
    # Angular 14's webpack chokes on Node 17+ OpenSSL3 (ERR_OSSL_EVP_UNSUPPORTED);
    # the device/cloud Dockerfiles avoid it by pinning node:18. Scarthgap's
    # nodejs-native is newer, so opt into the legacy provider and relax the
    # CLI engine check rather than failing the build.
    export NODE_OPTIONS="--openssl-legacy-provider"
    export npm_config_engine_strict=false
    # Reproducible, lockfile-pinned install; fall back if no cache.
    npm ci --no-audit --no-fund --prefer-offline || \
        npm install --no-audit --no-fund
    # angular.json already sets outputPath=dist/iot-ui
    npx --no-install ng build --configuration production --progress=false
}
addtask build_ui after do_patch do_prepare_recipe_sysroot before do_configure
do_build_ui[network] = "1"
do_build_ui[depends] += "nodejs-native:do_populate_sysroot"

# ── Sub-module cmake already uses GNUInstallDirs. Systemd + env files
# are overridden below from our WORKDIR copies so ExecStart uses /usr/bin
# rather than /usr/local/bin.
do_install() {
    cmake_do_install

    # OTA helpers: iot-ota-stage (download+verify+stage+trigger, run detached via
    # systemd-run by the lwm2m client) and iot-swupdate (the inotify-triggered
    # installer, run by iot-swupdate.service). See apps/docs/tdd-yocto-swupdate.md.
    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/iot-ota-stage    ${D}${bindir}/iot-ota-stage
    install -m 0755 ${WORKDIR}/iot-swupdate     ${D}${bindir}/iot-swupdate
    install -m 0755 ${WORKDIR}/iot-ota-confirm  ${D}${bindir}/iot-ota-confirm

    # Config/schema migration scripts (§11), run by iot-swupdate after opkg.
    install -d ${D}${datadir}/iot/migrations
    install -m 0644 ${WORKDIR}/migrations/README.md \
        ${D}${datadir}/iot/migrations/README.md
    install -m 0644 ${WORKDIR}/migrations/0000-template.sh.example \
        ${D}${datadir}/iot/migrations/0000-template.sh.example

    # ── systemd units (overwrite cmake-installed copies) ──────────
    if ${@bb.utils.contains('PACKAGECONFIG', 'systemd', 'true', 'false', d)}; then
        install -d ${D}${systemd_system_unitdir}
        install -m 0644 ${WORKDIR}/iot-ds.service            ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-lwm2m-client.service   ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-lwm2m-server.service   ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-openvpn-client.service ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-vpn-cert.path          ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-vpn-cert.service       ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-swupdate.path          ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-swupdate.service       ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-ota-confirm.service    ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-net-router.service     ${D}${systemd_system_unitdir}/
        # TUN driver autoload for openvpn-client.
        install -d ${D}${sysconfdir}/modules-load.d
        install -m 0644 ${WORKDIR}/iot-tun.conf               ${D}${sysconfdir}/modules-load.d/
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

# Bake an optional WiFi credential seed into the wifi.networks schema default.
# cmake_do_install has already placed wifi.lua under ds-schemas/; rewrite it in
# place from files/wifi_credentials.lua when that file was fetched. No-op (and
# the "changeme" placeholder default stands) when it is absent.
# python3-native gives the do_install task a python3 on PATH for the generator.
do_install[depends] += "python3-native:do_populate_sysroot"
do_install:append() {
    if [ -f ${WORKDIR}/wifi_credentials.lua ]; then
        python3 ${WORKDIR}/gen_wifi_default.py \
            ${WORKDIR}/wifi_credentials.lua \
            ${D}${sysconfdir}/iot/ds-schemas/wifi.lua \
            || bbfatal "gen_wifi_default.py failed on wifi_credentials.lua"
        bbnote "wifi-client: seeded wifi.networks default from wifi_credentials.lua"
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
# Each daemon ships its own systemd unit(s): SYSTEMD_SERVICE registers them
# for enable/disable but does NOT add them to FILES, and the per-package FILES
# below override the default ${PN} globs — so claim the units explicitly or
# do_package fails "installed but not shipped". (Harmless when the systemd
# PACKAGECONFIG is off: FILES may list paths that weren't installed.)
FILES:${PN}-ds-server = "\
    ${bindir}/ds-server \
    ${systemd_system_unitdir}/iot-ds.service \
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
    ${bindir}/iot-ota-stage \
    ${bindir}/iot-swupdate \
    ${bindir}/iot-ota-confirm \
    ${datadir}/iot/migrations \
    ${systemd_system_unitdir}/iot-lwm2m-client.service \
    ${systemd_system_unitdir}/iot-lwm2m-server.service \
    ${systemd_system_unitdir}/iot-swupdate.path \
    ${systemd_system_unitdir}/iot-swupdate.service \
    ${systemd_system_unitdir}/iot-ota-confirm.service \
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
    ${systemd_system_unitdir}/iot-openvpn-client.service \
    ${systemd_system_unitdir}/iot-vpn-cert.path \
    ${systemd_system_unitdir}/iot-vpn-cert.service \
    ${sysconfdir}/modules-load.d/iot-tun.conf \
"
# kernel-module-tun provides the TUN driver; iot-tun.conf autoloads it so
# /dev/net/tun exists before the client starts.
RDEPENDS:${PN}-openvpn-client = "ace-tao openvpn kernel-module-tun"
RRECOMMENDS:${PN}-openvpn-client = "\
    ${PN}-ds-server \
    ${PN}-config \
"

# net-router — nftables + iproute2 network daemon
FILES:${PN}-net-router = "\
    ${bindir}/net-router \
    ${systemd_system_unitdir}/iot-net-router.service \
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
    ${systemd_system_unitdir}/iot-wifi-client.service \
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
# Ships the Angular device UI (SPA) under ${datadir}/iot/www — served by
# iot-httpd via www-dir. Built by do_build_ui above.
FILES:${PN}-httpd = "\
    ${bindir}/iot-httpd \
    ${systemd_system_unitdir}/iot-httpd.service \
    ${datadir}/iot/www \
"
RDEPENDS:${PN}-httpd = "ace-tao"
RRECOMMENDS:${PN}-httpd = "\
    ${PN}-ds-server \
    ${PN}-config \
"

# config — schema files, env files, config templates (shared substrate)
# Includes the tmpfiles.d drop-in (creates /run/iot); it lives under
# ${nonarch_libdir} (not ${sysconfdir}), and since FILES:${PN} is overridden
# above the default tmpfiles pattern no longer ships it — claim it here so
# do_package doesn't fail with "installed but not shipped".
FILES:${PN}-config = "\
    ${sysconfdir}/iot \
    ${nonarch_libdir}/tmpfiles.d/iot.conf \
"

# ── systemd ─────────────────────────────────────────────────────────
SYSTEMD_SERVICE:${PN}-ds-server = "iot-ds.service"
# iot-swupdate.path is enabled (watches the OTA spool trigger); iot-swupdate.service
# is activated by the path unit, not enabled directly (it has no [Install]).
SYSTEMD_SERVICE:${PN}-lwm2m = "iot-lwm2m-client.service iot-lwm2m-server.service iot-swupdate.path iot-ota-confirm.service"
# iot-vpn-cert.path is enabled (watches the cert); iot-vpn-cert.service is
# activated by the path unit, not enabled directly (it has no [Install]).
SYSTEMD_SERVICE:${PN}-openvpn-client = "iot-openvpn-client.service iot-vpn-cert.path"
SYSTEMD_SERVICE:${PN}-net-router = "iot-net-router.service"
SYSTEMD_SERVICE:${PN}-wifi-client = "iot-wifi-client.service"
SYSTEMD_SERVICE:${PN}-httpd = "iot-httpd.service"

# ds-server and wifi-client auto-start. wifi-client comes up on every boot
# and reads wifi.networks from the data-store (schema default seeds a
# placeholder network); it parks in "disconnected" until provisioned. The
# unit already orders After=iot-ds.service network-online.target, so the
# schema default resolves before its first read. Other role units are
# enabled by the operator after writing the matching .env file — see DEPLOY.md.
SYSTEMD_AUTO_ENABLE:${PN}-ds-server = "enable"
SYSTEMD_AUTO_ENABLE:${PN}-lwm2m = "disable"
SYSTEMD_AUTO_ENABLE:${PN}-openvpn-client = "disable"
SYSTEMD_AUTO_ENABLE:${PN}-net-router = "disable"
SYSTEMD_AUTO_ENABLE:${PN}-wifi-client = "enable"
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
