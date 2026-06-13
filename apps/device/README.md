# apps/device — IoT Device stack (containerised, no RPi3B)

Build and run the **device-side** IoT stack in containers for development
and end-to-end testing **without a real Raspberry Pi 3B board**. It mirrors
`apps/cloud/` (Dockerfile + docker-compose.yml + run.sh) but builds the
device binaries and the device UI (`iot-ui/`) instead of the cloud ones.

The same daemons that run on a board via systemd run here as separate
containers sharing the ds-server Unix socket:

```
ds-server ── lwm2m-server (BS 5685 + DM 5683)
   │             ▲
   │             │ bootstrap + register (plain CoAP)
   │         lwm2m-client (local 5684)
   │
   └──────── iot-httpd (device REST API + device UI, :8080 → host :8081)
                 all IPC via /run/iot/data_store.sock
```

## Build

The image is built locally (not published). Build from this directory —
the build context is the repo root so it can see `modules/`, `apps/`,
`packaging/`, and `iot-ui/`:

```bash
cd apps/device
./run.sh build        # → naushada/iot-device:latest
```

Or directly:

```bash
docker build -t naushada/iot-device:latest -f apps/device/Dockerfile .
```

The `lwm2m` binary is built with `-DIOT_ENABLE_MONGO=OFF` (the device
MongoDB registration mirror isn't needed for container e2e — the same
flag the cloud image uses). Drop the flag for a mongo-enabled build.

## Run

```bash
./run.sh              # start all services
./run.sh logs         # tail all logs (or: ./run.sh logs lwm2m-client)
./run.sh ps           # list services
./run.sh ds get log.lwm2m.text   # read a ds key via ds-cli
./run.sh stop         # stop all
```

By default this is a **self-contained loop**: the local `lwm2m-server`
plays the LwM2M authority and the `lwm2m-client` bootstraps + registers
against it over plain CoAP — a working registration cycle with zero
external dependencies.

| Port | Service |
|------|---------|
| 8081 (host) → 8080 | Device UI + REST API |

CoAP ports stay internal to the compose network (not published) so the
device stack can run **alongside the cloud stack** on the same host
without port clashes.

## Test against the real cloud

Start the cloud stack (`apps/cloud/run.sh`) — it publishes the Bootstrap
server on host `5684` and DM on `5683` — then start the device and
provision its bootstrap target in the data store (it is **not** an env
var; ds is the single source of truth and persists in the `dev-lib`
volume):

```bash
./run.sh
# point the client at the cloud BS (one-time; persists in ds):
./run.sh ds set iot.bs.uri '"coaps://host.docker.internal:5684"'
./run.sh ds set iot.bs.psk.key '"…"'    # see apps/cloud/CLAUDE.md
# (or commission interactively in device-ui: ./run.sh commission on)
```

`host.docker.internal` resolves to the host via the `host-gateway`
mapping already wired into the client service. The **VPN server endpoint**
(`vpn.remote.host/port/proto`) is not set here at all — the cloud pushes it
over LwM2M Object 2048 after the device registers, and openvpn-client
hot-reloads on the change.

## Environment knobs

| Var | Default | Meaning |
|-----|---------|---------|
| `DEVICE_IMAGE` | `docker.io/naushada/iot-device:latest` | image tag |
| `HTTP_PORT` | `8081` | host port for the device UI |
| `RESET_CONFIG` | `1` | refresh the `dev-etc` config volume on start |

> The bootstrap target (`iot.bs.uri`) and VPN endpoint (`vpn.remote.*`) are
> **data-store** values, not env vars — see above. `iot.bs.uri` is set once
> (commissioning / `ds set`, persisted in `dev-lib`); `vpn.remote.*` is
> cloud-pushed via LwM2M Object 2048.

## Relationship to the other device build paths

| Path | Purpose |
|------|---------|
| `apps/device/` (this) | **Dev / e2e** — full stack as compose services, no board |
| `docker/Dockerfile` | Fat dev image (`naushada/iot:latest`) — full toolchain |
| `packaging/Containerfile` | Slim single-binary runtime image (`IOT_ROLE=…`) |
| `yocto/build.sh` | Real RPi3B bootable `.wic.bz2` + `.ipk` feed |
