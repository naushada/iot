# ============================================================
# Stage 1: Build C++ modules
# ============================================================
FROM docker.io/library/ubuntu:22.04 AS builder
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get -o Acquire::Check-Date=false update && \
    apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config autoconf \
    libssl-dev zlib1g-dev libreadline-dev \
    liblua5.3-dev lua5.3 wget ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# ACE/TAO 7.0.0 (~60s)
RUN wget -q https://github.com/DOCGroup/ACE_TAO/releases/download/ACE%2BTAO-7_0_0/ACE+TAO-7.0.0.tar.gz \
    -O /tmp/ace.tar.gz && cd /tmp && tar xzf ace.tar.gz && cd ACE_wrappers && \
    echo '#include "ace/config-linux.h"' > ace/config.h && \
    echo 'include $(ACE_ROOT)/include/makeinclude/platform_linux.GNU' > include/makeinclude/platform_macros.GNU && \
    export ACE_ROOT=/tmp/ACE_wrappers && make -j$(nproc) -C ace && \
    mkdir -p /usr/local/ACE_TAO-7.0.0/include /usr/local/ACE_TAO-7.0.0/lib && \
    cp -r ace/ /usr/local/ACE_TAO-7.0.0/include/ && \
    cp ace/libACE.so.* /usr/local/ACE_TAO-7.0.0/lib/ && \
    ln -sf libACE.so.7.0.0 /usr/local/ACE_TAO-7.0.0/lib/libACE.so && \
    ldconfig && echo /usr/local/ACE_TAO-7.0.0/lib > /etc/ld.so.conf.d/ace.conf && \
    rm -rf /tmp/ACE_wrappers /tmp/ace.tar.gz

# ── VPN PKI: per-build CA + server cert (xpmile pattern) ─────
# CA key is generated and used to sign server + client certs,
# then PURGED so it never lands in the final image.  iot-cloudd
# generates per-device client certs at provision time by reading
# ca.key from a mounted secret volume at runtime.
RUN mkdir -p /etc/iot/vpn /etc/iot/vpn/ca && cd /tmp && \
    openssl genrsa -out ca.key 4096 && \
    openssl req -new -x509 -days 3650 -key ca.key -out ca.crt \
        -subj "/O=IoT Cloud/CN=iot-cloud-ca" && \
    openssl genrsa -out server.key 2048 && \
    openssl req -new -key server.key -out server.csr \
        -subj "/O=IoT Cloud/CN=cloud-vpn" && \
    openssl x509 -req -days 3650 -in server.csr \
        -CA ca.crt -CAkey ca.key -CAcreateserial -out server.crt && \
    cp ca.crt /etc/iot/vpn/ca/ca.crt && \
    cp server.crt server.key /etc/iot/vpn/ && \
    rm -f server.csr ca.srl && \
    echo "VPN PKI generated — CA valid 10 years, server valid 10 years"

COPY modules/ /src/modules/
COPY apps/ /src/apps/
COPY apps/cloud/server/ /src/apps/cloud/server/

# Build data-store (needs ACE + Lua)
RUN mkdir -p /tmp/bds && cd /tmp/bds && \
    cmake /src/modules/data-store \
    -DCMAKE_BUILD_TYPE=Release \
    -DACE_ROOT=/usr/local/ACE_TAO-7.0.0 \
    -DLUA_INCLUDE_DIR=/usr/include/lua5.3 \
    -GNinja && ninja -j$(nproc)

# Build http-server (needs data-store client + ACE + OpenSSL)
RUN mkdir -p /tmp/bhttp && cd /tmp/bhttp && \
    cmake /src/modules/http-server \
    -DCMAKE_BUILD_TYPE=Release \
    -DACE_ROOT=/usr/local/ACE_TAO-7.0.0 \
    -DLUA_INCLUDE_DIR=/usr/include/lua5.3 \
    -DBUILD_HTTP_SERVER_TESTS=OFF \
    -GNinja && ninja -j$(nproc)

# Build cloud server daemon (iot-cloudd) — links against ds lib
RUN mkdir -p /tmp/bcloud && cd /tmp/bcloud && \
    cmake /src/apps/cloud/server \
    -DCMAKE_BUILD_TYPE=Release \
    -DACE_ROOT=/usr/local/ACE_TAO-7.0.0 \
    -DDATASTORE_LIB=/tmp/bds/libdatastore_client.a \
    && make -j2

# Build vendored tinydtls — produces libtinydtls.a, which the lwm2m
# target links. autoconf → autoheader → configure → make libtinydtls.a.
# The repo .gitignore excludes apps/3rdparty/tinydtls/doc/, so its
# autoconf inputs (doc/Makefile.in, doc/Doxyfile.in) aren't in the build
# context; stub them so configure completes (we don't build the docs).
RUN cd /src/apps/3rdparty/tinydtls && \
    mkdir -p doc && \
    [ -f doc/Makefile.in ] || : > doc/Makefile.in && \
    [ -f doc/Doxyfile.in ] || : > doc/Doxyfile.in && \
    autoconf && autoheader && ./configure && \
    make libtinydtls.a -j$(nproc)

# Build LwM2M binary (reused in server role for BS + DM).
# IOT_ENABLE_MONGO=OFF is now cleanly supported by apps/CMakeLists.txt
# (the else branch clears BSONCXX_LINK_LIB and MONGOCXX_LINK_LIB).
RUN mkdir -p /tmp/blwm2m && cd /tmp/blwm2m && \
    cmake /src/apps \
    -DCMAKE_BUILD_TYPE=Release \
    -DACE_ROOT=/usr/local/ACE_TAO-7.0.0 \
    -DLUA_INCLUDE_DIR=/usr/include/lua5.3 \
    -DIOT_ENABLE_MONGO=OFF \
    -GNinja && ninja -j$(nproc)

# ============================================================
# Stage 2: Build Angular cloud UI (xpmile pattern)
# ============================================================
FROM docker.io/library/node:18-alpine AS ui-builder
ENV NG_BUILD_MAX_WORKERS=1
ENV NODE_OPTIONS=--max_old_space_size=1024

WORKDIR /ui
COPY apps/cloud/ui/package.json apps/cloud/ui/package-lock.json ./
RUN npm ci 2>/dev/null || npm install --no-audit --no-fund

COPY apps/cloud/ui/ ./
RUN npx ng build --configuration production --base-href /webui/ --progress=false

# ============================================================
# Stage 3: Minimal runtime (xpmile pattern)
# ============================================================
FROM docker.io/library/ubuntu:22.04 AS runtime

RUN apt-get -o Acquire::Check-Date=false update && \
    apt-get install -y --no-install-recommends \
    libssl3 zlib1g libreadline8 liblua5.3-0 iptables openvpn ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# ACE shared libraries
COPY --from=builder /usr/local/ACE_TAO-7.0.0/lib/libACE.so* /usr/local/lib/
RUN ldconfig

# Binaries
COPY --from=builder /tmp/bds/ds-server /usr/local/bin/
COPY --from=builder /tmp/bds/ds-cli /usr/local/bin/
COPY --from=builder /tmp/bcloud/iot-cloudd /usr/local/bin/
COPY --from=builder /tmp/bhttp/iot-httpd /usr/local/bin/
COPY --from=builder /tmp/blwm2m/lwm2m /usr/local/bin/

# Static assets — both cloud UI (webui/) and device UI (webui/) served
# by iot-httpd depending on www-dir.
COPY --from=ui-builder /ui/dist/cloud-ui /usr/share/iot/cloud-www/webui/

# Schemas (device + cloud — same ds-server loads both)
COPY modules/data-store/schemas/iot.lua /etc/iot/ds-schemas/
COPY modules/data-store/schemas/services.lua /etc/iot/ds-schemas/
COPY modules/http-server/schemas/http.lua /etc/iot/ds-schemas/
COPY modules/http-server/schemas/auth.lua /etc/iot/ds-schemas/
COPY modules/server/lwm2m/schemas/cloud.lua /etc/iot/ds-schemas/
COPY modules/net/router/schemas/net.lua /etc/iot/ds-schemas/
COPY modules/openvpn/client/schemas/vpn.lua /etc/iot/ds-schemas/
COPY modules/wan/wifi/client/schemas/wifi.lua /etc/iot/ds-schemas/

# Device configs (LwM2M objects — reused by lwm2m binary in server role)
COPY apps/config/ /etc/iot/config/

RUN mkdir -p /var/run/iot /var/lib/iot

EXPOSE 8080 5683 5684 1194

# Default: start ds-server then iot-httpd (single-container mode).
# docker-compose overrides CMD per service.
CMD ds-server --socket=/var/run/iot/data_store.sock \
    --schema-dir=/etc/iot/ds-schemas \
    --persist-dir=/var/lib/iot & \
    sleep 2 && \
    exec iot-httpd ds-socket=/var/run/iot/data_store.sock \
        http-port=8080 www-dir=/usr/share/iot/cloud-www
