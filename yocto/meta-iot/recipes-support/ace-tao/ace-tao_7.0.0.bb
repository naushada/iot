SUMMARY = "ACE/TAO 7.0.0 — Adaptive Communication Environment"
DESCRIPTION = "ACE reactor and OS-abstraction library used by all iot daemons. \
Builds the core ACE library (not TAO, Kokyu, or other upper services). \
Uses its own GNUmakefile build system; the recipe writes config.h and \
platform_macros.GNU at configure time then drives `make -C ace`."
HOMEPAGE = "https://github.com/DOCGroup/ACE_TAO"
LICENSE = "DOC-ACE"
LIC_FILES_CHKSUM = "file://ACE-INSTALL.html;beginline=102;endline=115;md5=668a2e2fd4837f04043230c2c7e1ffa0"
SECTION = "libs"

# Fetch the official GitHub release tarball (~40 MB) rather than a git
# clone of the full repo (~1.2 GB). The tarball is the same artefact
# used in docker/Dockerfile.
SRC_URI = "https://github.com/DOCGroup/ACE_TAO/releases/download/ACE%2BTAO-7_0_0/ACE+TAO-7.0.0.tar.gz"
# DOCGroup re-published the 7.0.0 release asset upstream, so its bytes (and
# thus sha256) changed from the original pin (0e4d4a32…). The value below
# was verified by two independent downloads of the official release URL
# (41384345 bytes, valid gzip of ACE_wrappers/).
SRC_URI[sha256sum] = "75bc65d77d3f9dec716adcb37c4ff29658d5a5691d6430d8d3a6fac5b45d238b"

S = "${WORKDIR}/ACE_wrappers"

DEPENDS = "openssl"

# ACE's GNUmakefile system respects CC/CXX from the environment but needs
# them as make variables. The standard autotools/cmake classes don't apply.
EXTRA_OEMAKE = "\
    ACE_ROOT='${S}' \
    INSTALL_PREFIX='${D}${prefix}' \
    ssl=1 \
    SSL_ROOT='${STAGING_INCDIR}/openssl' \
    CC='${CC}' \
    CXX='${CXX}' \
    AR='${AR}' \
    LD='${CXX}' \
    RANLIB='${RANLIB}' \
    CFLAGS='${CFLAGS}' \
    CXXFLAGS='${CXXFLAGS}' \
    LDFLAGS='${LDFLAGS}' \
    debug=0 \
    optimize=0 \
"

do_configure() {
    # ACE bootstraps its build by reading ace/config.h and
    # include/makeinclude/platform_macros.GNU. Both are tiny includes
    # that delegate to ACE's built-in platform definitions.
    echo '#include "ace/config-linux.h"' > ${S}/ace/config.h
    cat > ${S}/include/makeinclude/platform_macros.GNU <<EOF
include \$(ACE_ROOT)/include/makeinclude/platform_linux.GNU
ssl=1
debug=0
optimize=0
EOF
}

do_compile() {
    oe_runmake -C ${S}/ace
}

do_install() {
    oe_runmake -C ${S}/ace install

    # Remove TAO, Kokyu, and other services we don't need.
    rm -rf ${D}${prefix}/include/tao
    rm -rf ${D}${prefix}/include/orbsvcs
    rm -rf ${D}${prefix}/share
}

# ── Package split ──────────────────────────────────────────────────────
# libACE.so.7.0.0 + libACE_SSL.so.7.0.0 are the runtime surface.
# Headers and .so symlinks go in -dev; static archives in -staticdev.

PACKAGES = "${PN} ${PN}-dev ${PN}-staticdev ${PN}-dbg"

FILES:${PN} = "\
    ${libdir}/libACE.so.* \
    ${libdir}/libACE_SSL.so.* \
"

FILES:${PN}-dev = "\
    ${includedir}/ace \
    ${libdir}/libACE.so \
    ${libdir}/libACE_SSL.so \
"

FILES:${PN}-staticdev = "\
    ${libdir}/libACE.a \
    ${libdir}/libACE_SSL.a \
"

BBCLASSEXTEND = "native nativesdk"
