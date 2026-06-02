SUMMARY = "tinydtls 0.8.6 — lightweight DTLS library"
DESCRIPTION = "Eclipse tinydtls is a minimal DTLS 1.2 implementation for \
constrained devices. The iot LwM2M stack links it statically for CoAPs \
(CoAP over DTLS-PSK) transport. This recipe fetches from the \
legatoproject fork — the same source pinned in apps/3rdparty/tinydtls/."
HOMEPAGE = "https://github.com/legatoproject/tinydtls"
LICENSE = "EPL-1.0 | EDL-1.0"
LIC_FILES_CHKSUM = "file://COPYING;md5=815ca599c9df247a0c7f619bab123dad"
SECTION = "libs"

# Fetch from the same legatoproject fork that .gitmodules pins.
# The SRCREV should match the submodule pin in the iot repo. When the
# submodule advances, update this SRCREV.
SRC_URI = "git://github.com/legatoproject/tinydtls.git;protocol=https;branch=master \
           file://0001-security-and-uthash-fixes.patch \
          "
SRCREV = "3bdb972e3f4f832bada96839c9d36501eb8e299b"

S = "${WORKDIR}/git"

inherit autotools

# tinydtls ships without a pre-built configure script. Generate it at
# configure time (matches the autoconf && autoheader step in Dockerfile).
do_configure:prepend() {
    cd ${S}
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
