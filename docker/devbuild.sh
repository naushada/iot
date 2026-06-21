#!/usr/bin/env bash
# Compile + run the apps/ gtest suite (lwm2m_test) from a mounted working tree,
# inside the localhost/iot-devbuild image. No GitHub clone, no mongo — the test
# suite omits the mongo/UDP/DTLS-bring-up wiring (see apps/test/CMakeLists.txt).
#
#   podman run --rm -v "$PWD":/src:Z -w /src localhost/iot-devbuild:latest \
#       bash docker/devbuild.sh [gtest_filter]
#
# Optional arg = a --gtest_filter (e.g. 'Senml*'). Builds out-of-tree under
# /tmp/iot-build so the mounted tree stays clean.
set -euo pipefail

SRC=/src
BUILD=/tmp/iot-build
FILTER="${1:-*}"

echo "== tinydtls (in-tree, required by apps link) =="
cd "$SRC/apps/3rdparty/tinydtls"
if [ ! -f libtinydtls.a ]; then
    autoconf && autoheader && ./configure >/dev/null && make >/dev/null 2>&1
fi

echo "== cmake configure (BUILD_APPS_TESTS=ON, IOT_ENABLE_MONGO=OFF) =="
rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"
cmake "$SRC/apps" -DBUILD_APPS_TESTS=ON -DIOT_ENABLE_MONGO=OFF >/tmp/cmake.log 2>&1 \
    || { echo "!! cmake failed"; tail -40 /tmp/cmake.log; exit 1; }

echo "== build lwm2m_test =="
make -j"$(nproc)" lwm2m_test 2>/tmp/make.log \
    || { echo "!! build failed"; tail -60 /tmp/make.log; exit 1; }

echo "== run: --gtest_filter=$FILTER =="
./test/lwm2m_test --gtest_filter="$FILTER"
