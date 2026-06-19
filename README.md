# iot — OMA LwM2M 1.1.1 Stack

A C++17 implementation of the OMA Lightweight M2M (LwM2M) device
management protocol on top of CoAP / DTLS, with the I/O layer built on
ACE (`ACE_Reactor`, `ACE_Task`, `ACE_SOCK_Dgram`).

> **License:** MIT — see [`LICENSE`](LICENSE). For a product overview,
> deployment fit, security posture, and an honest list of current
> limitations, see [`SALES.md`](SALES.md). (Distributed images bundle
> third-party components under their own licenses — see `SALES.md` §6.)

One binary, `lwm2m`, plays either role:

| Role     | Listens on                                  | Roughly speaking            |
|----------|---------------------------------------------|------------------------------|
| `server` | `local=` Bootstrap port + `:5683` DM port   | LwM2M Server + Bootstrap-Server |
| `client` | `local=` LwM2M Client port                  | LwM2M Client (device)        |

## Status (2026-06-03)

| Phase | Closes (RDD §6) | State |
|------:|-----------------|-------|
| L1 | ObjectStore + TLV codec carve-out | ✅ |
| L2 | link-format printer + Discover | ✅ |
| L3 | Registration Client + Server + ClientRegistry + mirror skeleton | ✅ |
| L4 | Bootstrap Client + Server FSMs (BUG-001 fixed) | ✅ |
| L5 | Read / Write / Create / Delete / Execute via Resource callbacks | ✅ |
| L6 | SenML JSON + CBOR codecs (BUG-002 fixed) | ✅ |
| L7 | Observe / Notify + threshold engine (D4 NON-default / CON-on-demand) | ✅ |
| L8 | Canonical objects + Linux platform readers + JSON-backed Device | ✅ |
| L9 | App-level wiring + Leshan interop harness + first pcap pass | ✅ |
| L10 | Data-store: typed values (variant), EMP framing, schema, live watch + hot-apply for `iot.{endpoint, lifetime, server.uri}` | ✅ |
| L11 | Packaging: systemd units, GNUInstallDirs install rules, OCI runtime image, end-to-end smoke, [`DEPLOY.md`](DEPLOY.md) | ✅ |
| L16 | `services.*.enable` control plane — per-daemon enable/state gate via ds-server | ✅ |
| L17 | Yocto / OpenEmbedded layer (`meta-iot`) — containerised multi-arch build (x86-64, ARM64, ARMv7) | ✅ |
| L17a | Dependency graph — `depends_on` schema, `DepWatch` helper, `gate.reason="dep_down:<name>"` | ✅ |
| L17b | Ephemeral disable — `ds-cli svc disable --until-boot`, in-memory volatile overlay | ✅ |
| L17c | Per-key ACL — `write_acl`/`read_acl` in schema, Unix peer credential enforcement | ✅ |
| L17d | Rate-limit + chaos coverage — set-rate throttling, random gate-flip harness | ✅ |
| L18 | HTTP REST API server (`iot-httpd`) — AF_UNIX data-store bridge, worker pool, TLS/mTLS, hot-reload | ✅ |
| L19 | Auth module — SHA-256 session cookies, role-based access control (Admin/Viewer), log ring buffer, keep-alive + idle timeout | ✅ |
| L20 | **IoT Manager UI** — Angular 14 SPA with Clarity Design System, collapsible sidebar, long-poll real-time updates, dark/light theme, Debug mode (reveals the data-store key + editable raw value under each form field) | ✅ |

All six binding decisions (D1–D6) are recorded in
[`apps/docs/lwm2m-rdd.md`](apps/docs/lwm2m-rdd.md#11-decisions-log) and
mirrored in [`apps/docs/lwm2m-design.md`](apps/docs/lwm2m-design.md#12-decisions).

## IoT Manager UI

A single-page management dashboard at `iot-ui/`.  Serve it with `--www-dir`
in `iot-httpd`, or run the dev server with `./serve.sh` (podman required).

| Section    | Purpose |
|------------|---------|
| Dashboard  | Live VPN/WiFi/WAN/LwM2M connection-status cards (real-time LwM2M bootstrap → device-management → registered lifecycle) + service overview |
| VPN        | OpenVPN config form + real-time connection status |
| WAN        | WiFi config, scan results, interface priority |
| Routing    | DNAT target + port forwarding rules |
| LwM2M      | Server URI, endpoint, binding, lifetime |
| Services   | Enable/disable toggles, restart buttons |
| Logs       | Scrollable live log viewer + log level control |

**Quick start:**
```sh
cd iot-ui && ./serve.sh          # http://localhost:4200
./serve.sh build                 # production build → dist/iot-ui/
```

## Build

Two paths land everything (`lwm2m` + `ds-server` + `ds-cli` +
`libdatastore_client.a` + tests):

- **Dev image** (`docker/Dockerfile` → `naushada/iot:latest`) — fat
  build environment with ACE_TAO 7.0.0, OpenSSL 3.1.1, mongo-c/cxx,
  tinydtls, gtest. Use this for compiling and running tests.
- **Runtime image** (`packaging/Containerfile` → `iot:l11`) — thin
  multi-stage image for deployment. ~119 MB. See
  [`packaging/README.md`](packaging/README.md).

For a full deploy walkthrough (container + bare-metal paths +
operator cookbook), see [`DEPLOY.md`](DEPLOY.md).

### Dev image (podman or docker)

```sh
podman build -t naushada/iot:latest -f docker/Dockerfile .
```

The image:
- `ubuntu:jammy` base.
- Steps 1–7 install ACE_TAO 7.0.0 to `/usr/local/ACE_TAO-7.0.0`.
- Steps 8–9 build OpenSSL 3.1.1 from `naushada/openssl` fork.
- Steps 10–11 build `mongo-c-driver r1.19` and `mongo-cxx-driver v3.6`.
- Steps 12–13 clone googletest + the project, build tinydtls via
  autoconf, then `cmake .. && make` for the `lwm2m` + `lwm2m_test`
  targets.

### Local (no container)

Needs the same deps installed on the host:

```sh
cd apps/3rdparty/tinydtls && autoconf && autoheader && ./configure && make
cd ../.. && mkdir -p build && cd build && cmake .. && make
```

For a packageable install:

```sh
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
make -C build -j"$(nproc)"
make -C build install DESTDIR=/tmp/staging   # or sudo make install for /
```

See [`DEPLOY.md`](DEPLOY.md) for the resulting layout + systemd
integration.

## Quick-start

Plain CoAP, server side:

```sh
./lwm2m local=coap://0.0.0.0:5783 role=server ep=urn:dev:client-1 \
        config=../config
```

Plain CoAP, client side (with auto-Register fallback for non-Bootstrap targets):

```sh
./lwm2m local=coap://0.0.0.0:56830 bs=coap://<server>:5683 role=client \
        ep=urn:dev:client-1 config=../config
```

PSK / DTLS variant — supply `identity=…` and `secret=…` (16-byte hex PSK):

```sh
./lwm2m local=coaps://0.0.0.0:5684 role=server \
        identity=97554878B284CE3B727D8DD06E87659A \
        secret=3894beedaa7fe0eae6597dc350a59525 \
        ep=urn:dev:client-1
```

> This shared `identity=/secret=` is a **manual dev/interop** override only. The
> cloud deployment hardcodes no PSK: each endpoint is provisioned with its own
> key in `cloud.endpoint.credentials`, and the server resolves it live from the
> data store at the handshake (identity derived from the serial). See
> `apps/cloud/CLAUDE.md` → *LwM2M CoAP Server*.

CLI flags:

| Flag       | Required | Meaning                                                                  |
|------------|:--------:|--------------------------------------------------------------------------|
| `local`    | yes      | `coap[s]://host:port` to bind                                            |
| `role`     | yes      | `server` or `client`                                                     |
| `bs`       | client   | `coap[s]://host:port` of the Bootstrap or LwM2M Server                   |
| `identity` | DTLS     | PSK identity (string)                                                    |
| `secret`   | DTLS     | PSK 16-byte key in hex (32 chars)                                        |
| `ep`       | no       | Endpoint name (defaults to `urn:dev:client-1`)                           |
| `config`   | no       | Directory containing `securityObject/`, `serverObject/`, `deviceObject/` |

## Run the test binary

```sh
./apps/build/test/lwm2m_test
```

The unit-test target deliberately doesn't link tinydtls or ACE — the
~120 GoogleTest cases exercise pure-C++ logic (codecs, FSMs, registry,
observe engine, object store). Runtime regressions for DTLS / network I/O
are owned by the Leshan interop pass, not the unit-test build.

## Yocto / OpenEmbedded build

A complete [Yocto](https://www.yoctoproject.org/) layer (`meta-iot`)
ships under `yocto/meta-iot/`. It builds a **full bootable distribution**
(`iot-image`) for the **Raspberry Pi 3B** — and the per-daemon `.ipk`
feed — with no host-side Yocto install. The entire build runs inside a
podman/docker container.

```sh
cd yocto
./build.sh                           # raspberrypi3-64 image (default)
MACHINE=qemuarm64 ./build.sh         # ARM64 qemu image
TARGET=packagegroup-iot ./build.sh   # just the .ipk feed, no image
./build.sh all                       # every supported machine
```

The host needs only **podman** or **docker**. The container image
clones poky + meta-openembedded + meta-raspberrypi at Scarthgap
(5.0 LTS), copies `meta-iot`, and runs `bitbake iot-image`. Output lands
in `yocto/build/<machine>/`:

- `images/<machine>/iot-image-*.wic.bz2` — flashable SD-card image
  (full gateway stack + kernel modules + Pi Wi-Fi/BT firmware + sshd + opkg).
  A stable `iot-image-<machine>.rootfs.wic.bz2` symlink alongside it always
  points at the newest build.
- `ipk/` — the `.ipk` feed for `opkg install` over ssh

Flash (find the SD device with `lsblk` / `diskutil list` first — write the
whole disk, not a partition), boot, ssh in, then push app updates over ssh:

```sh
bzcat yocto/build/raspberrypi3-64/images/raspberrypi3-64/iot-image-*.wic.bz2 \
  | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress   # macOS: /dev/rdiskN
ssh root@<pi-ip>                                   # sshd baked in (debug-tweaks)
scp yocto/build/raspberrypi3-64/ipk/*/iot-*.ipk root@<pi-ip>:/tmp/   # later updates
ssh root@<pi-ip> 'opkg install /tmp/iot-*.ipk'
```

In the field, app updates ride **LwM2M Object 5 (Firmware Update)** instead of
the manual `scp`/`opkg` above: the cloud points the device at an `.ipk` (or a
`.tar.gz` bundle of every `iot-*.ipk`, for a whole-userspace upgrade) in its
firmware feed and the `iot-ota-stage` helper pulls + verifies (sha256) it
into a tmpfs spool and trips an inotify trigger; the separate `iot-swupdate`
service then installs it, runs config/schema migrations, and restarts or reboots
— decoupled so it survives replacing the running binaries. The download runs
**direct over the public WAN, not the VPN tunnel** (the sha256 arrives over the
trusted DTLS control plane, so the payload transport needn't be trusted), with
retry+resume — so OTA works even when the tunnel is down and a bundle can safely
replace the VPN client itself. See the OTA section in
[`yocto/meta-iot/README.md`](yocto/meta-iot/README.md#ota-updates-lwm2m-object-5)
and the design in [`apps/docs/tdd-yocto-swupdate.md`](apps/docs/tdd-yocto-swupdate.md).

Full docs: [`yocto/meta-iot/README.md`](yocto/meta-iot/README.md) ·
deploy walkthrough: [`DEPLOY.md`](DEPLOY.md) Path C.

### PACKAGECONFIG

| Option   | Default | Effect |
|----------|---------|--------|
| `mongo`  | ON      | Links mongocxx/bsoncxx. RegistryMirror. Turn OFF for embedded IoT. |
| `gtest`  | OFF     | Builds unit-test binaries. |
| `systemd`| ON      | Installs systemd units + env files. OFF for sysvinit images. |

Override in `local.conf` or via kas:

```
PACKAGECONFIG:remove:pn-iot = "mongo"
```

### Package split

| Package                 | Binary                 | Runtime deps |
|-------------------------|------------------------|-------------|
| `iot-ds-server`         | `/usr/bin/ds-server`   | ACE, Lua |
| `iot-ds-cli`            | `/usr/bin/ds-cli`      | ACE |
| `iot-lwm2m`             | `/usr/bin/lwm2m`       | ACE, Lua, OpenSSL, tinydtls, ±mongo |
| `iot-openvpn-client`    | `/usr/bin/openvpn-client` | ACE, openvpn |
| `iot-net-router`        | `/usr/bin/net-router`  | ACE, nftables, iproute2 |
| `iot-wifi-client`       | `/usr/bin/wifi-client` | ACE, wpa-supplicant, udhcpc |
| `iot-config`            | `/etc/iot/`            | — |

Tiered `packagegroup-iot-{core,full,debug}` meta-packages pull in
what you need — from a minimal LwM2M device to a full gateway.

## Leshan interop pass (NFR-INTEROP-001)

Full procedure and acceptance matrices are in
[`apps/docs/leshan-interop.md`](apps/docs/leshan-interop.md). Quick path:

```sh
bash log/L9/run-interop-001.sh
```

This:
1. Brings up `corfr/leshan` server on the `lwm2m-interop` podman network.
2. Attaches a `nicolaka/netshoot` tcpdump sidecar to the Leshan netns
   (capability `NET_RAW` + `NET_ADMIN`).
3. Starts our binary in client mode pointed at Leshan.
4. Lets the Register + initial-tick window run for 75 s.
5. Tears down and leaves the capture at `log/L9/nfr-001-coap.pcap`.

The captured pcap shows our `POST /rd?ep=…&lt=86400&lwm2m=1.1&b=U` with a
75-byte link-format payload, and Leshan's `2.01 Created` reply carrying
the `Location-Path: rd/<id>` option pair — wire compliance for REQ-REG-001
and REQ-REG-002.

NFR-INTEROP-002 (Leshan client ↔ our server) is deferred — Docker Hub
does not host a Leshan client demo image, so it would need the Maven JAR
to be built from source. See `apps/docs/leshan-interop.md` §2.

## Layout

```
apps/                          LwM2M client + server binary
├── inc/                       public headers (LwM2M, codecs, registration / bootstrap / observe / DsConfig)
├── src/                       implementation files (+ cli/ for the REPL)
├── test/                      GoogleTest suites (154 tests)
├── config/                    Lua templates for Security/Server/Device/...
├── 3rdparty/                  vendored tinydtls + nlohmann::json
├── docs/
│   ├── architecture.md        post-L11 layout
│   ├── ace-refactor.md        I/O refactor history
│   ├── cli.md                 REPL commands
│   ├── lwm2m-design.md        module map + FSMs + decisions
│   ├── lwm2m-rdd.md           requirements catalogue + traceability
│   └── leshan-interop.md      L9 interop runbook
└── CMakeLists.txt

modules/
└── data-store/                AF_UNIX KV store (ds-server + ds-cli + libdatastore_client.a)
    ├── inc/data_store/        public client headers (POSIX-clean)
    ├── src/                   server + client + cli + proto
    ├── test/                  39 tests (proto framing, schema, persistence, watch)
    ├── schemas/iot.lua        canonical iot.* schema (installed to /etc/iot/ds-schemas/)
    └── docs/
        ├── design.md          design + history
        ├── protocol.md        EMP wire spec + opcodes + ds-cli examples
        ├── client_api.md      libdatastore_client integration guide
        └── tdd.md             D1–D10 phase plan + closures

packaging/                     L11 deploy artefacts
├── systemd/                   iot-{ds,lwm2m-client,lwm2m-server}.service + iot.conf (tmpfiles.d)
├── etc-iot/                   lwm2m-{client,server}.env templates
├── Containerfile              multi-stage OCI runtime build (~119 MB)
├── iot-entrypoint.sh          IOT_ROLE dispatcher
└── README.md                  packaging internals + size budget

yocto/                         L17 Yocto / OpenEmbedded layer
├── build.sh                   host-entry build script (podman/docker)
├── Containerfile              Yocto build container image
├── kas-iot.yml                kas configuration
└── meta-iot/                  meta-iot Yocto layer

docker/
├── Dockerfile                 dev build image (naushada/iot:latest)
└── docker-compose.leshan.yml  Leshan interop harness

log/
├── L9/                        pcap captures + interop run scripts
├── L10/                       data-store phase closure + smokes
├── L11/                       packaging phase plan + e2e smoke
├── L12/                       openvpn-client WAN-gate smokes
├── L13/                       net-router lifecycle smokes
├── L14/                       full-stack compose smokes
├── L15/                       wifi-client smokes
├── L16/                       services.* enable plane plan + smokes
├── L17a/                      dependency graph plan + smokes
├── L17b/                      ephemeral disable plan
├── L17c/                      per-key ACL plan
└── L17d/                      rate-limit + chaos harness

DEPLOY.md                      top-level deploy walkthrough (container + bare metal)
```

## Decisions (D1–D6)

These are binding; the docs and code reference them by ID.

| ID | Decision |
|----|----------|
| D1 | Target LwM2M 1.1.1 only — no 1.0 compatibility shim, no 1.2 features. |
| D2 | v1 ships single-server, but all per-server state keyed by Short Server ID from day one. |
| D3 | Hybrid in-memory + async MongoDB mirror for the server registry. |
| D4 | Notify defaults to Non-Confirmable; every 10th notify is promoted to CON. |
| D5 | The custom `/push`/`/set`/`/get`/`/execute` data plane is always compiled in. |
| D6 | Device Object reads `apps/config/deviceObject/0.json` with per-RID fallback to compiled-in constants; live RIDs always bind to Linux readers. |

Full rationale: [`apps/docs/lwm2m-rdd.md`](apps/docs/lwm2m-rdd.md#11-decisions-log).

## Known follow-ups

L10 / L11 leftovers (data-store + packaging):

- **FUP-DS-1** — Profile the persistor's fsync cost under load. Move to
  a dedicated ACE_Task worker if it shows up in CoAP latency.
- **FUP-DS-12** — Live CoAPs DTLS rebind on `iot.server.uri` change.
  Today the peer is swapped but the DTLS session isn't re-handshaked.
  Opens only if a CoAPs deployment surfaces.
- **FUP-L11-1** — OCI image size shrink (119 MB → < 100 MB target).
  Routes in [`packaging/README.md`](packaging/README.md) §"Image size".

Older items still open from L9:

- **RegistryMirror activation** — the worker is built but not started by
  `main.cpp` because the Mongo schema PR is pending.
- **Leshan client-side interop** — needs the `leshan-client-demo` JAR
  built from source; no canonical image on Docker Hub. See
  [`apps/docs/leshan-interop.md`](apps/docs/leshan-interop.md) §8.
