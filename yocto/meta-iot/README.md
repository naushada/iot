# meta-iot — Yocto Layer for the iot LwM2M Stack

Yocto/OpenEmbedded layer that builds the iot LwM2M 1.1.1 device management
stack and all its third-party dependencies. Seven daemons are delivered as
separate installable packages so embedded images pull only what they need,
plus a full bootable `iot-image` distribution.

## Quick-start (containerised build — no host Yocto install)

```sh
cd yocto
./build.sh                       # raspberrypi3-64 image, default
MACHINE=qemuarm64 ./build.sh     # ARM64 qemu
TARGET=packagegroup-iot ./build.sh   # just the .ipk feed, no image
```

`build.sh` builds the entire Yocto stack inside a container defined by
`Containerfile` (Ubuntu 22.04 + poky + meta-openembedded + meta-raspberrypi,
all at the Scarthgap 5.0 LTS branch). The host needs only **podman** or
**docker**. Per-machine output lands in `yocto/build/<machine>/`:

- `images/<machine>/iot-image-*.wic.bz2` — flashable SD-card image
- `ipk/` — the `.ipk` feed for `opkg install` over ssh

### Deploy to a Raspberry Pi 3B

```sh
# 1. Flash the SD card (verified write; or use dd — see build.sh summary)
bmaptool copy yocto/build/raspberrypi3-64/images/raspberrypi3-64/iot-image-*.wic.bz2 /dev/sdX

# 2. Boot the Pi, then ssh in (image runs sshd; debug-tweaks → empty root pw)
ssh root@<pi-ip>

# 3. Push app updates later over ssh via the .ipk feed
scp yocto/build/raspberrypi3-64/ipk/*/iot-*.ipk root@<pi-ip>:/tmp/
ssh root@<pi-ip> 'opkg install /tmp/iot-*.ipk'
```

## What this layer provides

| Recipe                   | Produces                                                    |
|--------------------------|-------------------------------------------------------------|
| `lwm2m_git.bb`           | Sub-packages: `iot-ds-server`, `iot-ds-cli`, `iot-lwm2m`, `iot-openvpn-client`, `iot-net-router`, `iot-wifi-client`, `iot-httpd`, `iot-config` |
| `images/iot-image.bb`    | Full bootable distribution (`packagegroup-iot-full` + kernel modules + RPi Wi-Fi/BT firmware + sshd + opkg) → `*.wic.bz2` |
| `ace-tao_7.0.0.bb`       | `libACE.so.7.0.0`, `libACE_SSL.so.7.0.0`, dev headers       |
| `tinydtls_git.bb`        | `libtinydtls.a` (static archive)                             |
| `mongo-c-driver_1.19.bb` | `libbson-1.0.so`, `libmongoc-1.0.so`                        |
| `mongo-cxx-driver_3.6.bb`| `libbsoncxx.so`, `libmongocxx.so`                            |
| `packagegroup-iot.bb`    | Meta-packages: `packagegroup-iot-core`, `-full`, `-debug`   |

## Package groups

| Group                      | Contents                                                    |
|----------------------------|-------------------------------------------------------------|
| `packagegroup-iot-core`    | ds-server + lwm2m + ds-cli + config (minimal device/fleet)  |
| `packagegroup-iot-full`    | core + httpd + openvpn-client + net-router + wifi-client + runtime deps |
| `packagegroup-iot-debug`   | full + test binaries (when PACKAGECONFIG[gtest] is on)      |

## PACKAGECONFIG options (lwm2m recipe)

| Option   | Default | Effect                                                      |
|----------|---------|-------------------------------------------------------------|
| `mongo`  | ON      | Links mongocxx/bsoncxx/mongoc/bson (~15 MB). RegistryMirror.|
| `gtest`  | OFF     | Builds unit-test binaries.                                  |
| `systemd`| ON      | Installs systemd units + env files. OFF for sysvinit images.|

Override in `local.conf` or kas config:

```
PACKAGECONFIG:remove:pn-lwm2m = "mongo"
```

## Using on a target

Once the `.ipk` packages are installed on the target:

```sh
# Edit env files to point at your LwM2M server
vi /etc/iot/lwm2m-client.env

# Enable and start
systemctl enable --now iot-ds.service iot-lwm2m-client.service

# Verify
ds-cli --socket=/run/iot/data_store.sock get iot.endpoint
```

## Layer dependencies

- `poky` (scarthgap) — core layer
- `meta-openembedded` (scarthgap) — `meta-oe` (lua, nlohmann-json, gtest),
  `meta-python`, `meta-networking` (openvpn)
- `meta-raspberrypi` (scarthgap) — `raspberrypi3-64` MACHINE, bootfiles,
  `pi-bluetooth`, rpidistro Wi-Fi/BT firmware

`Containerfile` clones all of these into the builder image; `kas-iot.yml`
declares the same set for the kas-driven path.

## Target machines

Default machine is `raspberrypi3-64` (Raspberry Pi 3B, 64-bit). For other
boards or qemu, override `MACHINE`:

```sh
MACHINE=qemux86-64 ./build.sh    # x86-64 qemu (CI)
MACHINE=qemuarm64  ./build.sh    # ARM64 qemu
./build.sh all                   # every machine in build.sh's MACHINES list
```
