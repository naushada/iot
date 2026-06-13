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
           file://0002-add-dtls-log-sink.patch \
           file://platform-inet.h \
           file://platform-types.h \
          "
SRCREV = "9ae4f917d7687df71d521803446b8a4e9e41f59d"

S = "${WORKDIR}/git"

inherit autotools

# tinydtls only supports an in-tree build (its Makefile.in has no VPATH
# handling); build where configure generates the Makefile. This MUST come
# after `inherit autotools` — the class sets B = ${WORKDIR}/build, and
# setting B before the inherit gets overridden by it (do_compile then runs
# make in the empty build dir → "no makefile found").
B = "${S}"

# Full override of the autotools do_configure (which runs autoreconf):
# tinydtls is autoconf-only (Makefile.in, no Makefile.am). Under autoreconf
# the aclocal pass makes AM_PROG_AR available, so the
# `m4_ifdef([AM_PROG_AR], …)` in configure.ac expands to it and then fails
# with "required file 'ar-lib' not found" (automake never runs to install
# it). Plain `autoconf` takes the `[AR=ar]` fallback instead. configure.ac
# has no AC_CANONICAL_*, so the cross ./configure needs no config.sub.
#
# Also install the platform shims master-swi's session.h pulls in
# (<platform/inet.h> + <platform/types.h>) — the legato build supplies
# these but the repo doesn't ship them for Linux.
do_configure() {
    cd ${S}
    install -d ${S}/platform
    install -m 0644 ${WORKDIR}/platform-inet.h  ${S}/platform/inet.h
    install -m 0644 ${WORKDIR}/platform-types.h ${S}/platform/types.h
    autoconf
    autoheader
    oe_runconf
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
