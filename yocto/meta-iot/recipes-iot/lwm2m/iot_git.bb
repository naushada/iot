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
    file://iot-sensord.service \
    file://iot-cellular-client.service \
    file://iot-vehicled.service \
    file://iot-can0-up.service \
    file://iot-containerd.service \
    file://iot-mqttd.service \
    file://iot-ddnsd.service \
    file://iot-pcap.service \
    file://10-iot-wired.network \
    file://05-iot-cellular-ecm.network \
    file://iot-wifi-client.service \
    file://iot-httpd.service \
    file://iot.conf \
    file://90-iot.preset \
    file://lwm2m-client.env \
    file://lwm2m-server.env \
    file://openvpn-client.env \
    file://net-router.env \
    file://wifi-client.env \
    file://httpd.env \
    file://pcap.env \
    file://iot-ota-stage \
    file://iot-ota-stage.path \
    file://iot-ota-stage.service \
    file://iot-swupdate \
    file://iot-swupdate.path \
    file://iot-swupdate.service \
    file://iot-ota-confirm \
    file://iot-ota-confirm.service \
    file://iot-reboot.path \
    file://iot-reboot.service \
    file://iot-factory-reset \
    file://iot-factory-reset.path \
    file://iot-factory-reset.service \
    file://iot-transfer \
    file://iot-transfer.path \
    file://iot-transfer.service \
    file://iot-bank-switch \
    file://migrations/README.md \
    file://migrations/0000-template.sh.example \
    file://gen_wifi_default.py \
    file://iot-set-hostname \
    file://iot-hostname.service \
    file://iot-http.avahi.service \
    file://iot-dump \
    file://iot-ds-seed \
    file://iot-ds-seed.service \
    file://bcm2837-selftest.service \
"

# Optional, gitignored WiFi credential seed. When enabled, files/wifi_credentials.lua
# is baked into the wifi.networks schema default at build time (see do_install:append
# + apps/docs/tdd-wifi-credentials-seed.md).
#
# Gate on a VARIABLE, never os.path.exists(): a parse-time filesystem probe made
# do_fetch's basehash non-deterministic — bitbake reparses (-Snone) and the hash
# flips when the (gitignored) file's presence differs between passes, failing with
# "the metadata is not deterministic". IOT_WIFI_SEED is part of the signature, so
# toggling it (or editing the fetched file's contents) still triggers a rebuild;
# the do_install:append below stays guarded on the file actually being fetched.
#
# Integrator: set IOT_WIFI_SEED = "1" (local.conf / kas) AND drop the file. Default
# off => no-op, the committed "changeme" placeholder stands.
IOT_WIFI_SEED ??= "0"
SRC_URI += "${@'file://wifi_credentials.lua' if d.getVar('IOT_WIFI_SEED') == '1' else ''}"

SRCREV = "${AUTOREV}"

# Package version. Keep the leading semver in sync with the repo-root /VERSION
# file (the single source of truth that iot-httpd also bakes into iot.version).
# PV can't read the fetched VERSION at parse time (source isn't unpacked yet),
# so it's set here manually. The +git${SRCPV} suffix gives opkg per-commit
# upgrade ordering (AUTOINC+sha) for the _git recipe.
PV = "1.4.0+git${SRCPV}"

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
    mosquitto \
    sqlite3 \
"
# sqlite3: DurableSampleBuffer — the on-device telemetry store-and-forward
# outbox (lwm2m_durable_sample_buffer.cpp). Not behind IOT_ENABLE_MONGO; the
# lwm2m binary always links it (runtime-gated by iot.telemetry.db.path).

# ── PACKAGECONFIG ──────────────────────────────────────────────────
# mongo:          Enables mongocxx/bsoncxx link (~15 MB in final image).
#                 Required for the RegistryMirror feature (db_adapter.cpp).
#                 Disable for resource-constrained embedded targets.
# gtest:          Builds the iot apps unit-test binaries. Dev-only; OFF by default.
# bcm2837-selftest: Builds the bcm2837 driver gtest suite (modules/bcm2837 submodule)
#                 and ships it as the bcm2837-selftest.service boot oneshot.
# systemd:        Installs systemd unit files + env files + tmpfiles.d.
#                 Auto-detected from DISTRO_FEATURES.
PACKAGECONFIG ??= "mongo bcm2837-selftest \
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

# bcm2837 driver self-test. When enabled, apps/CMakeLists.txt flips the bcm2837
# submodule's BCM2837_BUILD_TESTS on (building /usr/bin/bcm2837_test) and pulls in
# gtest. Disabled → only the bcm2837_driver static lib is built (no gtest dep).
PACKAGECONFIG[bcm2837-selftest] = "\
    -DIOT_BUILD_BCM2837_SELFTEST=ON, \
    -DIOT_BUILD_BCM2837_SELFTEST=OFF, \
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
#
# `engineer` is ALSO a real interactive login account (shell /bin/sh, home
# /home/engineer, password "engineer") for field/console + ssh access —
# alongside the debug-tweaks empty-root login. The shell + home come from
# USERADD_PARAM; the password is set at FIRST BOOT via chpasswd in
# pkg_postinst_ontarget below (plaintext → chpasswd hashes it), which sidesteps
# the fragile `$`-escaping of a baked `useradd -p '$6$...'` hash.
# SECURITY: a known credential baked into every image — a DEV/debug convenience
# (same posture as debug-tweaks). REMOVE for production: drop the postinst and
# restore --shell /usr/sbin/nologin (the daemon role doesn't need a shell).
USERADD_PACKAGES = "${PN}-lwm2m"
GROUPADD_PARAM:${PN}-lwm2m = "--system engineer; --system iot"
USERADD_PARAM:${PN}-lwm2m = "--system --create-home \
    --shell /bin/sh --gid engineer --groups iot \
    --comment 'IoT LwM2M client' engineer"

# First-boot: give `engineer` the login password "engineer". Runs as root on the
# target (ontarget), so chpasswd can write /etc/shadow; by first boot the user
# already exists (created by the useradd postinst above). Non-zero return re-runs
# it next boot, so a transient failure self-heals. See the SECURITY note above.
pkg_postinst_ontarget:${PN}-lwm2m() {
    echo 'engineer:engineer' | chpasswd
}

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
    -DMQTT_BUILD_DAEMON=ON \
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

    # OTA helpers: iot-ota-stage (download+verify+stage+trigger, run as root by
    # iot-ota-stage.service which iot-ota-stage.path fires when the unprivileged
    # lwm2m client drops /run/iot/update/stage.req) and iot-swupdate (the
    # inotify-triggered installer, run by iot-swupdate.service). See
    # apps/docs/tdd-yocto-swupdate.md.
    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/iot-ota-stage    ${D}${bindir}/iot-ota-stage
    install -m 0755 ${WORKDIR}/iot-swupdate     ${D}${bindir}/iot-swupdate
    install -m 0755 ${WORKDIR}/iot-ota-confirm  ${D}${bindir}/iot-ota-confirm
    # Factory-reset wipe script (run as root by iot-factory-reset.service, which
    # iot-factory-reset.path fires when the unprivileged iot-httpd drops
    # /run/iot/factory-reset.request). Reboot needs no script (systemctl reboot).
    install -m 0755 ${WORKDIR}/iot-factory-reset ${D}${bindir}/iot-factory-reset
    # Device-transfer wipe script (run as engineer by iot-transfer.service, which
    # iot-transfer.path fires when iot-httpd drops /run/iot/transfer.request).
    install -m 0755 ${WORKDIR}/iot-transfer      ${D}${bindir}/iot-transfer
    install -m 0755 ${WORKDIR}/iot-bank-switch  ${D}${bindir}/iot-bank-switch

    # iot-dump: operator/debug tool — dumps all data-store keys + values for
    # a module (e.g. `iot-dump iot-wifi-client`). Ships with ds-cli.
    install -m 0755 ${WORKDIR}/iot-dump         ${D}${bindir}/iot-dump

    # iot-ds-seed: first-boot ds seed (http.workers=2 so the device-ui
    # long-poll endpoints — /status and the Terminal shell — have worker
    # threads instead of stalling the inline reactor). Runs once, before
    # iot-httpd. Needs ds-cli at runtime (RDEPENDS below).
    install -m 0755 ${WORKDIR}/iot-ds-seed      ${D}${bindir}/iot-ds-seed

    # Config/schema migration scripts (§11), run by iot-swupdate after opkg.
    install -d ${D}${datadir}/iot/migrations
    install -m 0644 ${WORKDIR}/migrations/README.md \
        ${D}${datadir}/iot/migrations/README.md
    install -m 0644 ${WORKDIR}/migrations/0000-template.sh.example \
        ${D}${datadir}/iot/migrations/0000-template.sh.example

    # udhcpc lease hook — wifi-client points udhcpc at this (-s) to mirror the
    # DHCP lease (ip/mask/gateway/dns/lease/domain) into ds for the device UI.
    # See apps/docs/tdd-wifi-dhcp-lease.md.
    install -m 0755 ${S}/modules/wan/wifi/client/scripts/udhcpc-ds.script \
        ${D}${datadir}/iot/udhcpc-ds.script

    # ── systemd units (overwrite cmake-installed copies) ──────────
    if ${@bb.utils.contains('PACKAGECONFIG', 'systemd', 'true', 'false', d)}; then
        install -d ${D}${systemd_system_unitdir}
        install -m 0644 ${WORKDIR}/iot-ds.service            ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-lwm2m-client.service   ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-lwm2m-server.service   ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-openvpn-client.service ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-vpn-cert.path          ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-vpn-cert.service       ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-ota-stage.path         ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-ota-stage.service      ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-swupdate.path          ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-swupdate.service       ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-ota-confirm.service    ${D}${systemd_system_unitdir}/
        # device-ui Advanced -> Reboot / Factory Reset: root .path units watch a
        # trigger the unprivileged iot-httpd drops in /run/iot, then reboot /
        # wipe+reboot. See modules/http-server (POST /api/v1/system/*).
        install -m 0644 ${WORKDIR}/iot-reboot.path            ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-reboot.service         ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-factory-reset.path     ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-factory-reset.service  ${D}${systemd_system_unitdir}/
        # device-ui Advanced -> Transfer: engineer .path/.service wipe customer
        # creds + VPN trust, keep network (re-home to a new owner). Phase 1 of
        # apps/docs/tdd-device-transfer.md.
        install -m 0644 ${WORKDIR}/iot-transfer.path          ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-transfer.service       ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-net-router.service     ${D}${systemd_system_unitdir}/
        # iot-sensord: mangOH Yellow sensor producer (maps I2C, publishes
        # iot.sensor.*). NOT auto-enabled — it needs the mangOH board attached
        # and CAP_SYS_RAWIO, so an operator enables it on sensor-equipped units.
        install -m 0644 ${WORKDIR}/iot-sensord.service        ${D}${systemd_system_unitdir}/
        # iot-cellular-client: mangOH WP modem WAN + GPS producer. NOT
        # auto-enabled — needs the WP module + serial ports, so an operator
        # enables it on cellular-equipped units.
        install -m 0644 ${WORKDIR}/iot-cellular-client.service ${D}${systemd_system_unitdir}/
        # iot-vehicled + iot-can0-up: CAN/OBD-II telemetry producer. NOT
        # auto-enabled — needs a CAN controller, so an operator enables it on
        # vehicle/CAN-equipped units. iot-vehicled Wants= iot-can0-up (brings up can0).
        install -m 0644 ${WORKDIR}/iot-vehicled.service        ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-can0-up.service         ${D}${systemd_system_unitdir}/
        # iot-containerd: single-container runtime shim (crun-backed). Runs as
        # root (mounts overlayfs + drives crun). Enabled by default — idle until
        # the device-ui issues a pull/run command; needs crun (RDEPENDS).
        install -m 0644 ${WORKDIR}/iot-containerd.service      ${D}${systemd_system_unitdir}/
        # iot-mqttd: MQTT telemetry mirror. Enabled by default but parks until a
        # broker is configured (mqtt.broker.host), so it is harmless when unused.
        install -m 0644 ${WORKDIR}/iot-mqttd.service           ${D}${systemd_system_unitdir}/
        # iot-pcap: on-demand tcpdump capture, OFF by default (debug aid).
        install -m 0644 ${WORKDIR}/iot-pcap.service            ${D}${systemd_system_unitdir}/
        # networkd wired profile: RequiredForOnline=no so a cable-less eth0 does
        # not pin the system "offline" and park systemd-timesyncd on a WiFi-only
        # unit (no-RTC clock would stay stale → TLS/VPN fails). See the file header.
        install -d ${D}${sysconfdir}/systemd/network
        install -m 0644 ${WORKDIR}/10-iot-wired.network       ${D}${sysconfdir}/systemd/network/
        # Cellular WAN over the WP7702 ECM link: the module's DHCP hands an address
        # but no router option, so add the default route via 192.168.2.2 at metric
        # 300 (below wlan0). Sorts before 10-iot-wired.network to win the cdc_ether
        # match. See apps/docs/hw-bringup-wp7702-cellular-wan.md.
        install -m 0644 ${WORKDIR}/05-iot-cellular-ecm.network ${D}${sysconfdir}/systemd/network/
        # TUN driver autoload for openvpn-client.
        install -d ${D}${sysconfdir}/modules-load.d
        install -m 0644 ${WORKDIR}/iot-tun.conf               ${D}${sysconfdir}/modules-load.d/
        install -m 0644 ${WORKDIR}/iot-wifi-client.service    ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-httpd.service          ${D}${systemd_system_unitdir}/
        install -m 0644 ${WORKDIR}/iot-ds-seed.service        ${D}${systemd_system_unitdir}/

        # bcm2837 driver self-test oneshot (only when the PACKAGECONFIG is enabled;
        # the /usr/bin/bcm2837_test binary is produced by the bcm2837 submodule's
        # cmake install when -DIOT_BUILD_BCM2837_SELFTEST=ON).
        if ${@bb.utils.contains('PACKAGECONFIG', 'bcm2837-selftest', 'true', 'false', d)}; then
            install -m 0644 ${WORKDIR}/bcm2837-selftest.service ${D}${systemd_system_unitdir}/
        fi

        # tmpfiles.d: sole owner of /run/iot (units no longer use RuntimeDirectory=iot)
        install -d ${D}${nonarch_libdir}/tmpfiles.d
        install -m 0644 ${WORKDIR}/iot.conf ${D}${nonarch_libdir}/tmpfiles.d/

        # systemd preset: keep SYSTEMD_AUTO_ENABLE enables across `preset-all`
        # (which the image runs on first boot — without this they get reset to
        # the default "disable" policy).
        install -d ${D}${nonarch_libdir}/systemd/system-preset
        install -m 0644 ${WORKDIR}/90-iot.preset ${D}${nonarch_libdir}/systemd/system-preset/

        # EnvironmentFile for each daemon
        install -d ${D}${sysconfdir}/iot
        install -m 0644 ${WORKDIR}/lwm2m-client.env   ${D}${sysconfdir}/iot/
        install -m 0644 ${WORKDIR}/lwm2m-server.env   ${D}${sysconfdir}/iot/
        install -m 0644 ${WORKDIR}/openvpn-client.env ${D}${sysconfdir}/iot/
        install -m 0644 ${WORKDIR}/net-router.env     ${D}${sysconfdir}/iot/
        install -m 0644 ${WORKDIR}/wifi-client.env    ${D}${sysconfdir}/iot/
        install -m 0644 ${WORKDIR}/httpd.env          ${D}${sysconfdir}/iot/
        # pcap.env documents iot-pcap.service overrides; claimed by ${PN}-config
        # via its ${sysconfdir}/iot glob (the unit reads it with a leading `-`,
        # so it is optional).
        install -m 0644 ${WORKDIR}/pcap.env           ${D}${sysconfdir}/iot/

        # Zero-touch mDNS device-UI discovery: derive iot-<serial> hostname at
        # boot and advertise iot-httpd (_http._tcp:8080) via Avahi. See
        # apps/docs/tdd-wifi-zero-touch-mdns.md.
        install -m 0755 ${WORKDIR}/iot-set-hostname           ${D}${bindir}/iot-set-hostname
        install -m 0644 ${WORKDIR}/iot-hostname.service       ${D}${systemd_system_unitdir}/
        install -d ${D}${sysconfdir}/avahi/services
        install -m 0644 ${WORKDIR}/iot-http.avahi.service     ${D}${sysconfdir}/avahi/services/iot-http.service
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
    ${PN}-sensord \
    ${PN}-cellular \
    ${PN}-vehicle \
    ${PN}-containerd \
    ${PN}-mqtt \
    ${PN}-ddns \
    ${PN}-pcap \
    ${PN}-config \
    ${PN}-bcm2837-selftest \
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

# ds-cli — operator debug/admin CLI (+ iot-dump key-dump helper)
FILES:${PN}-ds-cli = "\
    ${bindir}/ds-cli \
    ${bindir}/iot-dump \
"
RDEPENDS:${PN}-ds-cli = "ace-tao"

# lwm2m — combined LwM2M client + server binary (role selected at CLI)
FILES:${PN}-lwm2m = "\
    ${bindir}/lwm2m \
    ${bindir}/iot-ota-stage \
    ${bindir}/iot-swupdate \
    ${bindir}/iot-ota-confirm \
    ${bindir}/iot-bank-switch \
    ${datadir}/iot/migrations \
    ${systemd_system_unitdir}/iot-lwm2m-client.service \
    ${systemd_system_unitdir}/iot-lwm2m-server.service \
    ${systemd_system_unitdir}/iot-ota-stage.path \
    ${systemd_system_unitdir}/iot-ota-stage.service \
    ${systemd_system_unitdir}/iot-swupdate.path \
    ${systemd_system_unitdir}/iot-swupdate.service \
    ${systemd_system_unitdir}/iot-ota-confirm.service \
"
# NOTE: do NOT add an explicit RDEPENDS on libsqlite3-0 (the SQLite
# DurableSampleBuffer). That is a debian-renamed shlib package name — valid as an
# installed .ipk but NOT build-time resolvable ("Nothing RPROVIDES libsqlite3-0").
# The shlib auto-dependency already adds it from the linked .so; the OTA bundle
# ships the .ipk (iot-bundle.bb IOT_BUNDLE_EXTRA_PKGS), so it resolves at install.
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
    ${sysconfdir}/systemd/network/10-iot-wired.network \
    ${sysconfdir}/systemd/network/05-iot-cellular-ecm.network \
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
    ${datadir}/iot/udhcpc-ds.script \
    ${systemd_system_unitdir}/iot-wifi-client.service \
"
# iot-ds-cli: the udhcpc lease hook writes the lease via ds-cli.
# iw: the lease hook runs `iw dev <iface> set power_save off` to stop the RPi
#     brcmfmac WiFi from idle-disassociating (link/IP/VPN/LwM2M flapping).
RDEPENDS:${PN}-wifi-client = "\
    ace-tao \
    wpa-supplicant \
    iw \
    ${PN}-ds-cli \
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
    ${bindir}/iot-set-hostname \
    ${bindir}/iot-ds-seed \
    ${bindir}/iot-factory-reset \
    ${bindir}/iot-transfer \
    ${systemd_system_unitdir}/iot-httpd.service \
    ${systemd_system_unitdir}/iot-hostname.service \
    ${systemd_system_unitdir}/iot-ds-seed.service \
    ${systemd_system_unitdir}/iot-reboot.path \
    ${systemd_system_unitdir}/iot-reboot.service \
    ${systemd_system_unitdir}/iot-factory-reset.path \
    ${systemd_system_unitdir}/iot-factory-reset.service \
    ${systemd_system_unitdir}/iot-transfer.path \
    ${systemd_system_unitdir}/iot-transfer.service \
    ${sysconfdir}/avahi/services/iot-http.service \
    ${datadir}/iot/www \
"
# avahi-daemon provides the mDNS responder that advertises the device UI
# (_http._tcp on iot-<serial>.local) for zero-touch discovery.
# ${PN}-ds-cli supplies ds-cli, used by iot-ds-seed.service at first boot.
RDEPENDS:${PN}-httpd = "ace-tao avahi-daemon ${PN}-ds-cli"
RRECOMMENDS:${PN}-httpd = "\
    ${PN}-ds-server \
    ${PN}-config \
"

# sensord — mangOH Yellow sensor producer (maps I2C, publishes iot.sensor.*)
# The unprivileged lwm2m client mirrors iot.sensor.* into IPSO objects; this
# daemon is the privileged producer. sensord.env ships in ${PN}-config (the
# ${sysconfdir}/iot glob). See apps/docs/tdd-mangoh-yellow-sensors.md.
FILES:${PN}-sensord = "\
    ${bindir}/iot-sensord \
    ${systemd_system_unitdir}/iot-sensord.service \
"
RDEPENDS:${PN}-sensord = "ace-tao"
RRECOMMENDS:${PN}-sensord = "\
    ${PN}-ds-server \
    ${PN}-config \
"

# cellular — mangOH Yellow WP modem WAN + GPS producer (cell.*/gps.*)
# The lwm2m client mirrors gps.* into LwM2M Object 6; net-router routes the
# cellular slot (wwan0). cell.lua schema + cellular-client.env ship in
# ${PN}-config. See apps/docs/tdd-mangoh-yellow-sensors.md §6.
FILES:${PN}-cellular = "\
    ${bindir}/cellular-client \
    ${systemd_system_unitdir}/iot-cellular-client.service \
"
RDEPENDS:${PN}-cellular = "ace-tao"

# vehicle — iot-vehicled CAN/OBD-II (ISO 15765-4) telemetry producer.
# Publishes vehicle.* to ds; the lwm2m client mirrors them into a Vehicle
# object + GPS Object 6 for the cloud map. vehicle.lua schema ships in
# ${PN}-config (${sysconfdir}/iot). systemd unit + can0 bring-up land in a
# follow-up PR; for now the binary ships and is run manually / on demand.
# See apps/docs/tdd-vehicle-telemetry.md.
FILES:${PN}-vehicle = "\
    ${bindir}/iot-vehicled \
    ${systemd_system_unitdir}/iot-vehicled.service \
    ${systemd_system_unitdir}/iot-can0-up.service \
"
RDEPENDS:${PN}-vehicle = "ace-tao"
RRECOMMENDS:${PN}-vehicle = "\
    ${PN}-ds-server \
    ${PN}-config \
"

# containerd — iot-containerd single-container runtime shim. Pulls an OCI image,
# mounts its layers (overlayfs) and drives crun, publishing container.* to ds for
# the device-ui. Runs as root. container.lua schema ships in ${PN}-config.
# RDEPENDS crun (from meta-virtualization). See apps/docs/tdd-device-containers.md.
FILES:${PN}-containerd = "\
    ${bindir}/iot-containerd \
    ${systemd_system_unitdir}/iot-containerd.service \
"
# curl: the registry-v2 HTTP transport (puller shells out to it, like the OTA
# path). openssl: libcrypto for blob sha256. zlib: gzip layer decompression.
# iproute2 + nftables: bridge-mode networking (veth/bridge/netns + masquerade).
# (openssl/zlib are also auto-added by the shlib scan; listed for clarity.)
RDEPENDS:${PN}-containerd = "ace-tao crun curl openssl zlib iproute2 nftables"
RRECOMMENDS:${PN}-containerd = "\
    ${PN}-ds-server \
    ${PN}-config \
"

# mqtt — iot-mqttd telemetry mirror to an operator MQTT broker (libmosquitto).
# Enabled by default; parks until mqtt.broker.host is configured. mqtt.lua
# schema ships in ${PN}-config.
FILES:${PN}-mqtt = "\
    ${bindir}/iot-mqttd \
    ${systemd_system_unitdir}/iot-mqttd.service \
"
RDEPENDS:${PN}-mqtt = "ace-tao"
RRECOMMENDS:${PN}-mqtt = "\
    ${PN}-ds-server \
    ${PN}-config \
"

# ddns — iot-ddnsd device-side Dynamic DNS updater. Keeps a public DNS A record
# on the device's current public IP through a pluggable provider backend
# (dyndns2/duckdns/cloudflare/route53). Ships the unit ENABLED but the daemon
# parks (ddns.state=disabled) until the operator sets ddns.enabled + provider
# creds via the device-ui. ddns.lua schema + ddnsd.env ship in ${PN}-config.
# See apps/docs/tdd-ddns.md.
FILES:${PN}-ddns = "\
    ${bindir}/iot-ddnsd \
    ${systemd_system_unitdir}/iot-ddnsd.service \
"
# curl: outbound HTTP(S) transport (IP-echo + provider updates), shelled via
# ACE_Process like the OTA/registry paths — no libcurl link dep. openssl:
# libcrypto for the Route53 SigV4 HMAC-SHA256 signer. ca-certificates: TLS trust
# store for the HTTPS calls.
RDEPENDS:${PN}-ddns = "ace-tao curl openssl ca-certificates"
RRECOMMENDS:${PN}-ddns = "\
    ${PN}-ds-server \
    ${PN}-config \
"

# pcap — iot-pcap.service: on-demand tcpdump ring-buffer capture of the LwM2M
# DTLS planes, for field debugging. No iot binary of its own — just the unit;
# tcpdump (+ libpcap, pulled transitively) is the runtime dep. Ships DISABLED
# (SYSTEMD_AUTO_ENABLE below + an explicit `disable` in 90-iot.preset); the
# operator starts it only while diagnosing. pcap.env ships in ${PN}-config.
FILES:${PN}-pcap = "\
    ${systemd_system_unitdir}/iot-pcap.service \
"
RDEPENDS:${PN}-pcap = "tcpdump"
RRECOMMENDS:${PN}-pcap = "\
    ${PN}-config \
"
# A real data path also wants a modem manager + tools on the image; left to the
# integrator per WP firmware (ModemManager / libqmi / usb-modeswitch).
RRECOMMENDS:${PN}-cellular = "\
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
    ${nonarch_libdir}/systemd/system-preset/90-iot.preset \
"

# bcm2837-selftest — the bcm2837 driver GoogleTest binary + boot oneshot. Populated
# only when the bcm2837-selftest PACKAGECONFIG is enabled; ALLOW_EMPTY keeps the
# package valid (and the SYSTEMD_SERVICE below empty) when it is disabled.
FILES:${PN}-bcm2837-selftest = "\
    ${bindir}/bcm2837_test \
    ${systemd_system_unitdir}/bcm2837-selftest.service \
"
ALLOW_EMPTY:${PN}-bcm2837-selftest = "1"
# No manual RDEPENDS on gtest. `gtest` is a build-time DEPENDS (added via the
# PACKAGECONFIG above, resolved through PROVIDES) — it is NOT a runtime package
# name, so an explicit RDEPENDS:...= "gtest" fails with "Nothing RPROVIDES
# 'gtest'". If bcm2837_test links libgtest.so, Yocto's shared-lib dependency scan
# adds the correct runtime package automatically.
INSANE_SKIP:${PN}-bcm2837-selftest = "already-stripped"

# ── systemd ─────────────────────────────────────────────────────────
SYSTEMD_SERVICE:${PN}-ds-server = "iot-ds.service"
# iot-ota-stage.path + iot-swupdate.path are enabled (they watch the OTA spool
# triggers); their .service units are activated by the path units, not enabled
# directly (they have no [Install]).
SYSTEMD_SERVICE:${PN}-lwm2m = "iot-lwm2m-client.service iot-lwm2m-server.service iot-ota-stage.path iot-swupdate.path iot-ota-confirm.service"
# iot-vpn-cert.path is enabled (watches the cert); iot-vpn-cert.service is
# activated by the path unit, not enabled directly (it has no [Install]).
SYSTEMD_SERVICE:${PN}-openvpn-client = "iot-openvpn-client.service iot-vpn-cert.path"
SYSTEMD_SERVICE:${PN}-net-router = "iot-net-router.service"
SYSTEMD_SERVICE:${PN}-wifi-client = "iot-wifi-client.service"
# httpd also carries the boot-time hostname unit (iot-<serial>) that makes the
# Avahi advert resolvable — zero-touch device-UI discovery.
# iot-reboot.path + iot-factory-reset.path are enabled (they watch the triggers
# iot-httpd drops in /run/iot for device-ui Advanced -> Reboot/Factory Reset);
# their .service units are path-activated, not enabled directly (no [Install]).
SYSTEMD_SERVICE:${PN}-httpd = "iot-httpd.service iot-hostname.service iot-ds-seed.service iot-reboot.path iot-factory-reset.path iot-transfer.path"
# iot-sensord registered but NOT auto-enabled: it needs the mangOH Yellow board
# + CAP_SYS_RAWIO, so on a board without it the daemon would Restart-loop. The
# operator `systemctl enable --now iot-sensord` on sensor-equipped hardware.
SYSTEMD_SERVICE:${PN}-sensord = "iot-sensord.service"
SYSTEMD_AUTO_ENABLE:${PN}-sensord = "disable"
# cellular-client is auto-enabled, but its unit carries
# ConditionPathExistsGlob=/dev/ttyUSB* — on a board with no WP module systemd
# skips it (inactive, not failed) instead of Restart-looping. Enabling it by
# default is what makes the device-ui APN field (cell.apn → AT+CGDCONT) actually
# reach the modem on cellular hardware without a hand `systemctl enable`.
# Keep in sync with 90-iot.preset, or first-boot `preset-all` re-disables it.
SYSTEMD_SERVICE:${PN}-cellular = "iot-cellular-client.service"
SYSTEMD_AUTO_ENABLE:${PN}-cellular = "enable"
# vehicle (iot-vehicled + can0 bring-up) registered but NOT auto-enabled: needs a
# CAN controller, so it would Restart-loop on a board without one. Operator
# enables it on vehicle/CAN-equipped hardware (Wants= pulls in iot-can0-up).
SYSTEMD_SERVICE:${PN}-vehicle = "iot-vehicled.service iot-can0-up.service"
SYSTEMD_AUTO_ENABLE:${PN}-vehicle = "disable"
# containerd enabled by default so the device-ui container feature works out of
# the box. It is idle (just watches ds) until an Admin issues a pull/run, so it
# is a no-op on devices that never use it. Listed in 90-iot.preset so preset-all
# keeps it enabled. Needs the kernel features (overlayfs, namespaces, cgroups)
# the linux-raspberrypi container.cfg fragment turns on, plus crun (RDEPENDS).
SYSTEMD_SERVICE:${PN}-containerd = "iot-containerd.service"
SYSTEMD_AUTO_ENABLE:${PN}-containerd = "enable"
# mqtt mirror enabled by default — it parks (no broker connection) until
# mqtt.broker.host is configured, so it is a no-op until the operator sets it up.
SYSTEMD_SERVICE:${PN}-mqtt = "iot-mqttd.service"
# ddns registered + auto-enabled: the daemon is cheap while parked (a timer that
# publishes ddns.state=disabled) and comes alive the moment the operator sets
# ddns.enabled, so no `systemctl enable` step is needed. Also `enable`d in
# 90-iot.preset so preset-all keeps it on across first boot.
SYSTEMD_SERVICE:${PN}-ddns = "iot-ddnsd.service"
SYSTEMD_AUTO_ENABLE:${PN}-ddns = "enable"
# pcap capture registered but NOT auto-enabled: a continuous packet capture is a
# debug aid, not a boot service (flash wear + captures user traffic). Operator
# `systemctl start iot-pcap` while diagnosing. Also `disable`d in 90-iot.preset
# so preset-all keeps it off on first boot.
SYSTEMD_SERVICE:${PN}-pcap = "iot-pcap.service"
SYSTEMD_AUTO_ENABLE:${PN}-pcap = "disable"

# ds-server and wifi-client auto-start. wifi-client comes up on every boot
# and reads wifi.networks from the data-store (schema default seeds a
# placeholder network); it parks in "disconnected" until provisioned. The
# unit already orders After=iot-ds.service network-online.target, so the
# schema default resolves before its first read.
#
# lwm2m, openvpn-client and net-router also auto-start so a freshly flashed
# device brings up the full DM/VPN chain with zero SSH. Each daemon self-gates
# on its `services.<name>.enable` data-store key and parks (rather than doing
# work) until provisioned, so the device-UI Services page can pause/resume them
# without touching systemd. Caveat: until certs (/etc/iot/vpn/*), PSK and
# vpn.remote.port/proto are provisioned, openvpn-client/lwm2m may Restart=on-
# failure loop; that is expected on an un-provisioned boot. See DEPLOY.md.
SYSTEMD_AUTO_ENABLE:${PN}-ds-server = "enable"
SYSTEMD_AUTO_ENABLE:${PN}-lwm2m = "enable"
SYSTEMD_AUTO_ENABLE:${PN}-openvpn-client = "enable"
SYSTEMD_AUTO_ENABLE:${PN}-net-router = "enable"
SYSTEMD_AUTO_ENABLE:${PN}-wifi-client = "enable"
# httpd auto-starts for zero-touch device-UI access (mDNS discovery via Avahi).
# Enabling this exposes the UI + REST API on 0.0.0.0:8080 to the LAN by default;
# lock down via http.listen.ip / auth for untrusted networks. Also enables the
# iot-hostname oneshot (sets iot-<serial> so Avahi advertises a stable name).
SYSTEMD_AUTO_ENABLE:${PN}-httpd = "enable"

# bcm2837 self-test oneshot. Conditional so disabling the PACKAGECONFIG leaves an
# empty package with no dangling SYSTEMD_SERVICE reference.
SYSTEMD_SERVICE:${PN}-bcm2837-selftest = "${@bb.utils.contains('PACKAGECONFIG', 'bcm2837-selftest', 'bcm2837-selftest.service', '', d)}"
SYSTEMD_AUTO_ENABLE:${PN}-bcm2837-selftest = "enable"

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
INSANE_SKIP:${PN}-sensord = "already-stripped"
INSANE_SKIP:${PN}-cellular = "already-stripped"
