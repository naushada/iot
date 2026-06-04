#!/bin/bash
set -e
echo "=== Installing deps ==="
apt-get update -qq && apt-get install -y -qq g++ make cmake git liblua5.3-dev wget libssl-dev 2>&1 | tail -2

if [ ! -f /usr/local/lib/libgtest.a ]; then
    cd /tmp && git clone -q https://github.com/google/googletest.git 2>/dev/null || true
    cd /tmp/googletest && mkdir -p build && cd build
    cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local 2>&1 | tail -1
    make -j$(nproc) install 2>&1 | tail -1
fi

export ACE_ROOT=/tmp/ACE_wrappers
if [ ! -f /usr/local/ACE_TAO-7.0.0/lib/libACE.so ]; then
    echo "=== Building ACE ==="
    cd /tmp
    if [ ! -f ace-src-done ]; then
        wget -q https://github.com/DOCGroup/ACE_TAO/releases/download/ACE%2BTAO-7_0_0/ACE+TAO-7.0.0.tar.gz
        tar -xzf ACE+TAO-7.0.0.tar.gz
        echo '#include "ace/config-linux.h"' > $ACE_ROOT/ace/config.h
        echo 'include $(ACE_ROOT)/include/makeinclude/platform_linux.GNU' > $ACE_ROOT/include/makeinclude/platform_macros.GNU
        touch ace-src-done
    fi
    cd $ACE_ROOT
    make -j$(nproc) install ssl=1 INSTALL_PREFIX=/usr/local/ACE_TAO-7.0.0 ACE_ROOT=$ACE_ROOT 2>&1 | tail -3
    echo /usr/local/ACE_TAO-7.0.0/lib > /etc/ld.so.conf.d/ace.conf; ldconfig
fi

SRC=/src; cd $SRC
rm -rf /tmp/http-full-build && mkdir -p /tmp/http-full-build && cd /tmp/http-full-build
cmake $SRC/modules/http-server -DBUILD_HTTP_SERVER_TESTS=ON -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -2
echo "=== Building all targets ==="
make -j$(nproc) 2>&1 | tail -5
echo "=== Checking ds-server binary ==="
ls -l data-store/ds-server 2>/dev/null || echo "ds-server not found"; find . -name ds-server 2>/dev/null
echo "=== Running handler tests ==="
./http-handler-tests 2>&1
