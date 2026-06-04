SUMMARY = "ACE/TAO 7.0.0 — Adaptive Communication Environment"
DESCRIPTION = "ACE reactor and OS-abstraction library used by all iot daemons. \
Builds the core ACE library (not TAO, Kokyu, or other upper services). \
Uses its own GNUmakefile build system; the recipe writes config.h and \
platform_macros.GNU at configure time then drives `make -C ace`."
HOMEPAGE = "https://github.com/DOCGroup/ACE_TAO"
LICENSE = "DOC-ACE"
# Track the canonical license file (ACE_wrappers/COPYING — the full DOC
# license text) rather than a line-range in ACE-INSTALL.html, whose line
# numbers shifted when DOCGroup re-published the 7.0.0 tarball. Whole-file
# md5 is stable unless the license itself changes.
LIC_FILES_CHKSUM = "file://COPYING;md5=d2c090e9c730fd91677782d8e2091d77"
# DOC-ACE is not an SPDX/generic license; map it to the in-tree COPYING so
# do_populate_lic has a license file (silences the license-exists QA warn).
NO_GENERIC_LICENSE[DOC-ACE] = "COPYING"
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
    LDFLAGS='${LDFLAGS} -L${S}/lib' \
    debug=0 \
    optimize=0 \
"
# ACE's wrapper_macros.GNU does `LDFLAGS += -L$(INSLIB)` (INSLIB =
# $(ACE_ROOT)/lib) so its sub-libraries (ACE_Compression, ACE_ETCL, …) can
# find the freshly-built libACE. Passing LDFLAGS on the make command line
# (above) overrides that append (a command-line var can't be appended to in
# the makefile), so we add -L${S}/lib ourselves — without it the sub-libs
# fail to link with "cannot find -lACE".

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

# ACE's own `make install` also installs (and perl-post-processes) its
# MPC/build-system tree under share/ace; that step assumes share/ace was
# populated and dies with "Directory nonexistent" in a packaging sysroot —
# and we throw share/ away anyway. Install the runtime libs + headers
# directly from the build tree instead. (do_compile builds libs into
# ${S}/lib and ACE's headers live under ${S}/ace.)
do_install() {
    install -d ${D}${libdir}
    # Versioned shared libs + their .so / .so.MAJOR symlinks.
    cp -a ${S}/lib/libACE.so*     ${D}${libdir}/
    cp -a ${S}/lib/libACE_SSL.so* ${D}${libdir}/

    # Public headers: the ace/ tree (*.h, *.inl, and the template *.cpp
    # files that headers #include), minus build artifacts. ACE_SSL headers
    # live under ace/SSL/.
    install -d ${D}${includedir}/ace
    cp -a ${S}/ace/. ${D}${includedir}/ace/
    find ${D}${includedir}/ace -depth -type d \
        \( -name '.shobj' -o -name '.obj' \) -exec rm -rf {} +
    find ${D}${includedir}/ace -type f \
        \( -name '*.o' -o -name '*.a' -o -name '*.so' -o -name '*.so.*' \
           -o -name 'GNUmakefile*' \) -delete
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
