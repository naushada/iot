#!/usr/bin/env bash
# Compile the iot-ddnsd daemon (integrated apps build) AND build+run the ddns
# gtest suite (standalone module build), inside the localhost/iot-devbuild image.
set -euo pipefail
SRC=/src

echo "======================================================================"
echo "A) iot-ddnsd daemon — integrated apps build (mongo OFF)"
echo "======================================================================"
B1=/tmp/iot-ddns-app; rm -rf "$B1"; mkdir -p "$B1"; cd "$B1"
cmake "$SRC/apps" -DIOT_ENABLE_MONGO=OFF >/tmp/cmakeA.log 2>&1 \
  || { echo "!! cmake(apps) failed"; tail -50 /tmp/cmakeA.log; exit 1; }
make -j"$(nproc)" iot-ddnsd 2>/tmp/makeA.log \
  || { echo "!! iot-ddnsd build FAILED"; tail -120 /tmp/makeA.log; exit 1; }
echo "   OK: $(find "$B1" -name iot-ddnsd -type f)"

echo "======================================================================"
echo "B) ddns gtest suite — standalone module build"
echo "======================================================================"
B2=/tmp/iot-ddns-mod; rm -rf "$B2"; mkdir -p "$B2"; cd "$B2"
cmake "$SRC/modules/ddns" -DDDNS_BUILD_TESTS=ON >/tmp/cmakeB.log 2>&1 \
  || { echo "!! cmake(module) failed"; tail -50 /tmp/cmakeB.log; exit 1; }
make -j"$(nproc)" 2>/tmp/makeB.log \
  || { echo "!! ddns_test build FAILED"; tail -120 /tmp/makeB.log; exit 1; }
echo "-- running ddns_test --"
./ddns_test
echo "ALL GREEN"
