#!/usr/bin/env bash
# Compile the apps/ tree from a mounted working tree, inside the
# localhost/iot-devbuild image. No GitHub clone, no mongo (IOT_ENABLE_MONGO=OFF).
#
#   podman run --rm -v "$PWD":/src:Z -w /src localhost/iot-devbuild:latest \
#       bash docker/devbuild.sh [gtest_filter]
#
# Default: build + run the gtest suite (lwm2m_test) — the suite omits the
# mongo/UDP/DTLS-bring-up wiring (see apps/test/CMakeLists.txt). Optional arg =
# a --gtest_filter (e.g. 'Senml*').
#
# DEVBUILD_DAEMON=1 additionally links the full `lwm2m` daemon target — a link
# check for daemon-only sources (e.g. src/main.cpp) the gtest target does not
# compile. Requires libsqlite3-dev in the image (DurableSampleBuffer); rebuild
# the image if it predates that: podman build -t localhost/iot-devbuild:latest \
#   -f docker/Dockerfile.devbuild docker/
#
# Builds out-of-tree under /tmp/iot-build so the mounted tree stays clean.
set -euo pipefail

SRC=/src
BUILD=/tmp/iot-build
FILTER="${1:-*}"

echo "== tinydtls (in-tree, required by apps link) =="
cd "$SRC/apps/3rdparty/tinydtls"
# A libtinydtls.a left in the mounted tree by a host (macOS) build is the wrong
# arch and silently fails the container link (a recurring gotcha — Docker/podman
# ignores .gitignore, so the host artifact rides along). Rebuild unless the
# existing archive is a Linux x86-64 ELF ar.
need_tinydtls=1
if [ -f libtinydtls.a ]; then
    obj=$(ar t libtinydtls.a 2>/dev/null | grep -m1 '\.o$' || true)
    if [ -n "$obj" ] && ar p libtinydtls.a "$obj" >/tmp/td_probe.o 2>/dev/null \
        && file /tmp/td_probe.o | grep -q 'ELF.*x86-64'; then
        need_tinydtls=0
    fi
fi
if [ "$need_tinydtls" = "1" ]; then
    echo "   (building libtinydtls.a fresh)"
    rm -f libtinydtls.a
    autoconf && autoheader && ./configure >/dev/null && make >/dev/null 2>&1
fi

echo "== cmake configure (BUILD_APPS_TESTS=ON, IOT_ENABLE_MONGO=OFF) =="
rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"
cmake "$SRC/apps" -DBUILD_APPS_TESTS=ON -DIOT_ENABLE_MONGO=OFF >/tmp/cmake.log 2>&1 \
    || { echo "!! cmake failed"; tail -40 /tmp/cmake.log; exit 1; }

if [ "${DEVBUILD_DAEMON:-0}" = "1" ]; then
    echo "== build lwm2m daemon (link validation) =="
    make -j"$(nproc)" lwm2m 2>/tmp/make-daemon.log \
        || { echo "!! daemon build failed"; tail -80 /tmp/make-daemon.log; exit 1; }
    ls -la ./lwm2m
fi

echo "== build lwm2m_test =="
make -j"$(nproc)" lwm2m_test 2>/tmp/make.log \
    || { echo "!! build failed"; tail -60 /tmp/make.log; exit 1; }

echo "== run: --gtest_filter=$FILTER =="
./test/lwm2m_test --gtest_filter="$FILTER"
