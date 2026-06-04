SUMMARY = "tinydtls 0.8.6 — lightweight DTLS library"
DESCRIPTION = "Eclipse tinydtls is a minimal DTLS 1.2 implementation for \
constrained devices. The iot LwM2M stack links it statically for CoAPs \
(CoAP over DTLS-PSK) transport. This recipe fetches from the \
legatoproject fork — the same source pinned in apps/3rdparty/tinydtls/."
HOMEPAGE = "https://github.com/legatoproject/tinydtls"
LICENSE = "EPL-1.0 | EDL-1.0"
# master-swi renamed COPYING -> LICENSE (same EPL-1.0/EDL-1.0 dual text).
LIC_FILES_CHKSUM = "file://LICENSE;md5=ffb073dbb36e7ec5e091047332f302c5"
SECTION = "libs"

# Fetch from the legatoproject fork. The original pin (3bdb972e, branch
# master) was rebased away upstream — master is no longer the default and
# that commit no longer exists. Track the current default branch
# (master-swi) at a fixed SRCREV. The local patch is re-based onto it
# (the handle_handshake NULL-guard is already upstream here, so dropped).
SRC_URI = "git://github.com/legatoproject/tinydtls.git;protocol=https;branch=master-swi \
           file://0001-security-and-uthash-fixes.patch \
           file://platform-inet.h \
           file://platform-types.h \
          "
SRCREV = "9ae4f917d7687df71d521803446b8a4e9e41f59d"

S = "${WORKDIR}/git"

inherit autotools

# tinydtls ships without a pre-built configure script. Generate it at
# configure time (matches the autoconf && autoheader step in Dockerfile).
# master-swi's session.h pulls <platform/inet.h> + <platform/types.h>,
# platform shims the legato build supplies but the repo doesn't ship for
# Linux — provide them here (same files the iot repo vendors).
do_configure:prepend() {
    cd ${S}
    install -d ${S}/platform
    install -m 0644 ${WORKDIR}/platform-inet.h  ${S}/platform/inet.h
    install -m 0644 ${WORKDIR}/platform-types.h ${S}/platform/types.h
    autoconf
    autoheader
}

# tinydtls only produces a static library.
do_install() {
    install -d ${D}${libdir}
    install -m 0644 ${B}/libtinydtls.a ${D}${libdir}/libtinydtls.a
    # Install headers so dependent recipes can find them via sysroot.
    install -d ${D}${includedir}/tinydtls
    install -m 0644 ${S}/*.h ${D}${includedir}/tinydtls/
}

# tinydtls is a static-library-only recipe. Keep the default ${PN}
# package (even though empty) so the recipe target resolves correctly.
ALLOW_EMPTY:${PN} = "1"

FILES:${PN}-staticdev = "\
    ${libdir}/libtinydtls.a \
"

FILES:${PN}-dev = "\
    ${includedir}/tinydtls \
"
