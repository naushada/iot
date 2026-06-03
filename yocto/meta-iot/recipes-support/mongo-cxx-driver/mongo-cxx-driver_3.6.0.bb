SUMMARY = "mongo-cxx-driver 3.6.0 — MongoDB C++ driver"
DESCRIPTION = "Official MongoDB C++ driver (bsoncxx + mongocxx). Required by \
the lwm2m binary for its RegistryMirror feature. Disable the `mongo` \
PACKAGECONFIG in the lwm2m recipe to drop this dependency for embedded \
targets that don't need registration mirroring."
HOMEPAGE = "https://github.com/mongodb/mongo-cxx-driver"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://LICENSE;md5=3b83ef96387f14655fc854ddc3c6bd57"
SECTION = "libs"

SRC_URI = "git://github.com/mongodb/mongo-cxx-driver.git;protocol=https;branch=releases/v3.6"
SRCREV = "34497fb71af5c2bcee1da6ca4ac24fcad3523f62"

S = "${WORKDIR}/git"

inherit cmake pkgconfig

DEPENDS = "mongo-c-driver openssl"

# BSONCXX_POLY_USE_MNMLSTC=1 is load-bearing. The upstream cmake
# defaults to BSONCXX_POLY_USE_STD on C++17, but the iot Dockerfile
# explicitly sets MNMLSTC. Changing this causes ABI breakage in
# headers included from db_adapter.cpp.
EXTRA_OECMAKE = "\
    -DBSONCXX_POLY_USE_MNMLSTC=1 \
    -DCMAKE_BUILD_TYPE=Release \
    -DBSONCXX_OUTPUT_NAME=bsoncxx \
    -DMONGOCXX_OUTPUT_NAME=mongocxx \
    -DENABLE_TESTS=OFF \
    -DENABLE_EXAMPLES=OFF \
    -DCMAKE_SKIP_RPATH=ON \
"

PACKAGE_BEFORE_PN = "${PN}-bsoncxx"

FILES:${PN}-bsoncxx = "\
    ${libdir}/libbsoncxx.so.* \
"

FILES:${PN} = "\
    ${libdir}/libmongocxx.so.* \
"

FILES:${PN}-dev = "\
    ${includedir}/bsoncxx \
    ${includedir}/mongocxx \
    ${libdir}/libbsoncxx.so \
    ${libdir}/libmongocxx.so \
    ${libdir}/pkgconfig \
    ${libdir}/cmake \
"
