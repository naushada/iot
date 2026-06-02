SUMMARY = "mongo-c-driver 1.19.0 — MongoDB C driver"
DESCRIPTION = "Official MongoDB C driver (libbson + libmongoc). Required by \
mongo-cxx-driver and optionally by the lwm2m binary for its RegistryMirror \
feature. Disable the `mongo` PACKAGECONFIG in the lwm2m recipe to drop this \
dependency for embedded targets that don't need registration mirroring."
HOMEPAGE = "https://github.com/mongodb/mongo-c-driver"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://COPYING;md5=3b83ef96387f14655fc854ddc3c6bd57"
SECTION = "libs"

SRC_URI = "git://github.com/mongodb/mongo-c-driver.git;protocol=https;branch=r1.19"
SRCREV = "f74f20e82f2e1dc8f8e5c5b924ca835b2e095c7b"

S = "${WORKDIR}/git"

inherit cmake pkgconfig

DEPENDS = "openssl zlib"

EXTRA_OECMAKE = "\
    -DENABLE_AUTOMATIC_INIT_AND_CLEANUP=OFF \
    -DENABLE_SSL=OPENSSL \
    -DENABLE_SASL=OFF \
    -DENABLE_SRV=OFF \
    -DENABLE_TESTS=OFF \
    -DENABLE_EXAMPLES=OFF \
    -DENABLE_UNINSTALL=OFF \
    -DCMAKE_SKIP_RPATH=ON \
"

PACKAGE_BEFORE_PN = "${PN}-bson"

FILES:${PN}-bson = "\
    ${libdir}/libbson-1.0.so.* \
"

FILES:${PN} = "\
    ${libdir}/libmongoc-1.0.so.* \
"

FILES:${PN}-dev = "\
    ${includedir}/libbson-1.0 \
    ${includedir}/libmongoc-1.0 \
    ${libdir}/libbson-1.0.so \
    ${libdir}/libbson-static-1.0.a \
    ${libdir}/libmongoc-1.0.so \
    ${libdir}/libmongoc-static-1.0.a \
    ${libdir}/pkgconfig \
    ${libdir}/cmake \
"
