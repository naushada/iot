# L18 — HTTP REST API server build + test image.
#
# Builds ACE + Lua + GTest, then compiles and runs the
# full http-server test suite (parser + router + handlers).
#
#   podman build -t iot-httpd:l18 -f modules/http-server/Dockerfile .
#   podman run --rm iot-httpd:l18
#
# Or run just the smoke:
#   podman run --rm --network=host iot-httpd:l18 bash log/L18/smoke.sh

FROM ubuntu:jammy

RUN apt-get update && apt-get -y upgrade && \
    apt-get install -y g++ make cmake git liblua5.3-dev wget libssl-dev

# ── GTest ──────────────────────────────────────────────────────
WORKDIR /tmp
RUN git clone -q https://github.com/google/googletest.git && \
    cd googletest && mkdir -p build && cd build && \
    cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local && \
    make -j$(nproc) install

# ── ACE/TAO 7.0.0 ─────────────────────────────────────────────
ENV ACE_ROOT=/tmp/ACE_wrappers ACE_PREFIX=/usr/local/ACE_TAO-7.0.0
WORKDIR /tmp
RUN wget -q https://github.com/DOCGroup/ACE_TAO/releases/download/ACE%2BTAO-7_0_0/ACE+TAO-7.0.0.tar.gz && \
    tar -xzf ACE+TAO-7.0.0.tar.gz && rm ACE+TAO-7.0.0.tar.gz && \
    echo '#include "ace/config-linux.h"' > $ACE_ROOT/ace/config.h && \
    echo 'include $(ACE_ROOT)/include/makeinclude/platform_linux.GNU' \
          > $ACE_ROOT/include/makeinclude/platform_macros.GNU
WORKDIR $ACE_ROOT
RUN make -j$(nproc) install ssl=1 INSTALL_PREFIX=$ACE_PREFIX ACE_ROOT=$ACE_ROOT && \
    echo "$ACE_PREFIX/lib" > /etc/ld.so.conf.d/ace.conf && ldconfig

# ── iot http-server ────────────────────────────────────────────
WORKDIR /src
COPY . .

RUN mkdir -p modules/http-server/build && cd modules/http-server/build && \
    cmake .. -DBUILD_HTTP_SERVER_TESTS=ON -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc) iot-httpd http-parser-tests http-router-tests \
                    http-handler-tests http-tls-tests

# Run the full test suite
CMD cd /src/modules/http-server/build && \
    echo "=== Parser ===" && ./http-parser-tests && \
    echo "=== Router ===" && ./http-router-tests && \
    echo "=== Handlers ===" && ./http-handler-tests && \
    echo "=== TLS ===" && ./http-tls-tests
