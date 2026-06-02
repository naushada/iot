# meta-iot — Yocto Layer for the iot LwM2M Stack

Yocto/OpenEmbedded layer that builds the iot LwM2M 1.1.1 device management
stack and all its third-party dependencies. Six daemons are delivered as
separate installable packages so embedded images pull only what they need.

## Quick-start (containerised build — no host Yocto install)

```sh
cd yocto
./build.sh
```

`build.sh` runs the entire Yocto build inside the
[ghcr.io/siemens/kas/kas](https://github.com/siemens/kas) container image.
The host needs only **podman** or **docker**. Output packages land in
`yocto/build/deploy/ipk/`.

## What this layer provides

| Recipe                   | Produces                                                    |
|--------------------------|-------------------------------------------------------------|
| `lwm2m_git.bb`           | Six sub-packages: `iot-ds-server`, `iot-ds-cli`, `iot-lwm2m`, `iot-openvpn-client`, `iot-net-router`, `iot-wifi-client`, `iot-config` |
| `ace-tao_7.0.0.bb`       | `libACE.so.7.0.0`, `libACE_SSL.so.7.0.0`, dev headers       |
| `tinydtls_git.bb`        | `libtinydtls.a` (static archive)                             |
| `mongo-c-driver_1.19.bb` | `libbson-1.0.so`, `libmongoc-1.0.so`                        |
| `mongo-cxx-driver_3.6.bb`| `libbsoncxx.so`, `libmongocxx.so`                            |
| `packagegroup-iot.bb`    | Meta-packages: `packagegroup-iot-core`, `-full`, `-debug`   |

## Package groups

| Group                      | Contents                                                    |
|----------------------------|-------------------------------------------------------------|
| `packagegroup-iot-core`    | ds-server + lwm2m + ds-cli + config (minimal device/fleet)  |
| `packagegroup-iot-full`    | core + openvpn-client + net-router + wifi-client + runtime deps |
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

- `poky` (scarthgap branch) — core layer
- `meta-openembedded` (scarthgap branch) — provides lua, nlohmann-json, gtest

These are fetched automatically by `kas-iot.yml` when using the containerised
build.

## Target machines

Default machine is `qemux86-64` for CI/verification. For a physical device,
override `MACHINE` in a kas include or `local.conf`:

```sh
MACHINE=raspberrypi4-64 ./build.sh
```
