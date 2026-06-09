# ============================================================
# IoT Device image — the full device-side stack for dev / e2e
# testing WITHOUT a real Raspberry Pi 3B board.
#
# Mirrors apps/cloud/Dockerfile: a multi-stage build that compiles
# the same C++ daemons + the device Angular UI (iot-ui/) into a slim
# Ubuntu 22.04 runtime image. docker-compose then runs the binaries
# as separate containers sharing the ds-server socket — exactly the
# layout that runs on a real board via systemd, just containerised.
#
#   Stage 1 (ui-builder) — Angular build of iot-ui/  → dist/iot-ui
#   Stage 2 (builder)    — ACE/TAO + tinydtls + cmake build of apps/
#   Stage 3 (runtime)    — slim image: binaries + device UI + config
#
# The lwm2m binary is built with -DIOT_ENABLE_MONGO=OFF: the device
# MongoDB registration mirror isn't needed for container e2e (the
# same flag the cloud image uses). Drop the flag for a mongo build.
#
# Build from the REPO ROOT (the build context must see modules/,
# apps/, packaging/, iot-ui/):
#   docker build -t naushada/iot-device:latest -f apps/device/Dockerfile .
# Or: cd apps/device && ./run.sh build
# ============================================================

# ============================================================
# Stage 1: Build the device Angular UI (iot-ui)
# ============================================================
FROM docker.io/library/node:18-alpine AS ui-builder
ENV NG_BUILD_MAX_WORKERS=1
ENV NODE_OPTIONS=--max_old_space_size=1024

WORKDIR /ui
COPY iot-ui/package.json iot-ui/package-lock.json ./
RUN npm ci 2>/dev/null || npm install --no-audit --no-fund

COPY iot-ui/ ./
# Device UI is served at the web root by iot-httpd (base-href "/"),
# unlike the cloud UI which lives under /webui/.
RUN npx ng build --configuration production --progress=false

# ============================================================
# Stage 2: Build C++ daemons (ds-server, lwm2m, iot-httpd, …)
# ============================================================
FROM docker.io/library/ubuntu:22.04 AS builder
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get -o Acquire::Check-Date=false update && \
    apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config autoconf \
    libssl-dev zlib1g-dev libreadline-dev \
    liblua5.3-dev lua5.3 wget ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# ACE/TAO 7.0.0 (~60s) — same install prefix apps/CMakeLists.txt expects.
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

COPY modules/    /src/modules/
COPY apps/       /src/apps/
COPY packaging/  /src/packaging/

# Drop the Angular build where modules/http-server/CMakeLists.txt looks
# for it (../../iot-ui/dist/iot-ui). When present at configure time,
# `make install` stages the SPA to /usr/share/iot/www automatically.
COPY --from=ui-builder /ui/dist/iot-ui/ /src/iot-ui/dist/iot-ui/

# Build vendored tinydtls → libtinydtls.a (the lwm2m target links it).
# The repo .gitignore excludes apps/3rdparty/tinydtls/doc/, so its
# autoconf inputs aren't in the build context; stub them so configure
# completes (we don't build the docs).
RUN cd /src/apps/3rdparty/tinydtls && \
    mkdir -p doc && \
    [ -f doc/Makefile.in ] || : > doc/Makefile.in && \
    [ -f doc/Doxyfile.in ] || : > doc/Doxyfile.in && \
    autoconf && autoheader && ./configure && \
    rm -f libtinydtls.a *.o aes/*.o ecc/*.o sha2/*.o && \
    make libtinydtls.a -j$(nproc)

# One cmake build of apps/ produces every device binary — apps/CMakeLists
# add_subdirectory()s data-store, openvpn/client, net/router, wan/wifi,
# and http-server — then `make install` stages binaries + schemas + env
# files + config + the SPA into /staging.
RUN mkdir -p /tmp/bapps && cd /tmp/bapps && \
    cmake /src/apps \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DACE_ROOT=/usr/local/ACE_TAO-7.0.0 \
        -DLUA_INCLUDE_DIR=/usr/include/lua5.3 \
        -DIOT_ENABLE_MONGO=OFF && \
    make -j$(nproc) && \
    make install DESTDIR=/staging

# Container deploys treat the image as the unit of upgrade, so rename the
# object-resource config templates from .lua.example back to .lua — the
# lwm2m binary's config=/etc/iot/config boots out of the box. Operators
# override by mounting their own /etc/iot/config tree.
RUN find /staging/etc/iot/config -name '*.lua.example' \
        -exec sh -c 'mv "$1" "${1%.example}"' _ {} \;

# Strip the binaries — saves ~20 MB across the set.
RUN strip /staging/usr/bin/ds-server \
          /staging/usr/bin/ds-cli \
          /staging/usr/bin/lwm2m \
          /staging/usr/bin/iot-httpd \
          /staging/usr/bin/openvpn-client \
          /staging/usr/bin/net-router \
          /staging/usr/bin/wifi-client

# ============================================================
# Stage 3: Minimal runtime
# ============================================================
FROM docker.io/library/ubuntu:22.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive

# Runtime libs the binaries link against, plus the userspace tools the
# device daemons shell out to: openvpn(8) (openvpn-client), nft + ip
# (net-router), wpa_supplicant + udhcpc (wifi-client).
RUN apt-get -o Acquire::Check-Date=false update && \
    apt-get install -y --no-install-recommends \
    libssl3 zlib1g libreadline8 liblua5.3-0 ca-certificates \
    openvpn nftables iproute2 wpasupplicant udhcpc wireless-tools \
    && rm -rf /var/lib/apt/lists/* /usr/share/doc/* /usr/share/man/*

# PSK-provisioning accounts — mirror packaging/systemd/iot-sysusers.conf so
# the container matches the SD-card/systemd deployment:
#   * `engineer` runs the LwM2M client; its ds connection carries
#     gid:engineer, matching the read/write ACLs on the PSK keys.
#   * `iot` is the ds-socket access group (ds-server chgrp's its 0660 socket
#     to it; `engineer` is a member so it can connect).
# httpd stays root and relies on iot.dev.mode for commissioning writes.
RUN groupadd --system iot && \
    groupadd --system engineer && \
    useradd --system --no-create-home --shell /usr/sbin/nologin --gid engineer engineer && \
    usermod -aG iot engineer

# ACE 7.0.0 shared library.
COPY --from=builder /usr/local/ACE_TAO-7.0.0/lib/libACE.so* /usr/local/lib/
RUN ldconfig

# Binaries.
COPY --from=builder /staging/usr/bin/ds-server      /usr/local/bin/
COPY --from=builder /staging/usr/bin/ds-cli         /usr/local/bin/
COPY --from=builder /staging/usr/bin/lwm2m          /usr/local/bin/
COPY --from=builder /staging/usr/bin/iot-httpd      /usr/local/bin/
COPY --from=builder /staging/usr/bin/openvpn-client /usr/local/bin/
COPY --from=builder /staging/usr/bin/net-router     /usr/local/bin/
COPY --from=builder /staging/usr/bin/wifi-client    /usr/local/bin/

# Config tree: ds-schemas/*.lua, *.env, config/**/*.lua  (all under /etc/iot).
COPY --from=builder /staging/etc/iot      /etc/iot
# Device UI (SPA) served by iot-httpd at www-dir=/usr/share/iot/www.
COPY --from=builder /staging/usr/share/iot /usr/share/iot

# Entrypoint dispatches on IOT_ROLE or forwards a binary invocation.
COPY packaging/iot-entrypoint.sh /usr/local/bin/iot-entrypoint
RUN chmod +x /usr/local/bin/iot-entrypoint

# ds-server binds its socket here; /var/lib/iot holds the persisted store.
RUN mkdir -p /run/iot /var/lib/iot && \
    chmod 0755 /run/iot && chmod 0755 /var/lib/iot

# VPN cert family (ca.crt/client.crt/client.key) is written here by the
# lwm2m client (LwM2M Object 2048) and read by openvpn-client. The client
# runs as `engineer`, so make the dir engineer-writable; a fresh
# dev-vpn-certs volume mounted at this path inherits the ownership.
RUN mkdir -p /etc/iot/vpn && chown engineer:iot /etc/iot/vpn && chmod 0775 /etc/iot/vpn

# 8080 device UI/REST · 5683 DM (server role) · 5684 client local · 5685 BS
# CoAP/DTLS ports are UDP (EXPOSE defaults to TCP); 8080 is the HTTP UI/API (TCP).
EXPOSE 8080 5683/udp 5684/udp 5685/udp

ENTRYPOINT ["/usr/local/bin/iot-entrypoint"]
CMD []
