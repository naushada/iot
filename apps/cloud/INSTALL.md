# IoT Cloud — Installation Guide

How to bring up the **IoT Cloud Server** (ds-server, iot-cloudd, iot-httpd,
lwm2m-bs, lwm2m-dm) on a fresh machine. Everything is driven by one script:
`apps/cloud/run.sh`.

There are two paths:

- **A. Run the prebuilt image** from Docker Hub (`naushada/iot-cloud`) —
  fastest, no local compile. Recommended for most installs.
- **B. Build the image locally** from source — for development or air-gapped
  hosts.

Both start the same multi-container stack via `docker compose`.

---

## 1. Prerequisites

- **Linux host** (x86-64 or arm64). A VM/cloud instance is fine.
- **Docker Engine** + the **Compose plugin** (`docker compose`, v2).
  `run.sh` prefers `docker`; it falls back to `podman` only if docker is
  absent.
  ```bash
  # Ubuntu/Debian quick install:
  curl -fsSL https://get.docker.com | sh
  sudo usermod -aG docker "$USER"   # log out/in so docker runs without sudo
  docker compose version            # verify the compose plugin is present
  ```
- **git** (to clone the repo for `run.sh` + compose file).
- **Open ports** on the host (defaults; configurable, see §5):

  | Port | Proto | Purpose |
  |------|-------|---------|
  | 8080 | tcp | Cloud UI + REST API |
  | 5684 | udp | LwM2M Bootstrap (CoAPs) |
  | 5683 | udp | LwM2M Device Management (CoAPs) |
  | 1194 | udp | OpenVPN (device tunnels, served by iot-cloudd) |

---

## 2. Clone the repo

```bash
git clone https://github.com/naushada/iot.git
cd iot/apps/cloud
```

> The vendored `tinydtls` lib is committed in-tree, so a plain clone is
> enough — no `git submodule` step is needed.

`run.sh` and `docker-compose.yml` live here in `apps/cloud/`. Run all commands
below from this directory.

---

## 3A. Install — run the prebuilt image (recommended)

`run.sh` pulls `docker.io/naushada/iot-cloud:latest` automatically if it isn't
present locally:

```bash
./run.sh
```

That's it. To pull explicitly first (e.g. to refresh `:latest`):

```bash
docker pull docker.io/naushada/iot-cloud:latest
./run.sh
```

## 3B. Install — build the image locally

```bash
./run.sh build      # builds docker.io/naushada/iot-cloud:latest from source
./run.sh            # start the stack
```

`./run.sh nocache` forces a clean rebuild. The build compiles ACE, the C++
daemons, tinydtls, and the Angular UI inside the image (multi-stage), so the
first build takes several minutes.

---

## 4. Verify it's up

```bash
./run.sh ps         # list the 5 containers (should be Up / healthy)
./run.sh logs       # daemon log tail + service states
```

Then open the dashboard:

- **Cloud UI:** http://<host-ip>:8080
- **Login:** `admin` / `admin`

The **Services** page shows each daemon's state plus live CPU % / #CPU /
Memory / FDs / Threads.

---

## 5. Configuration (env vars)

Override on the command line — they pass through to `docker-compose.yml`:

| Variable | Default | Meaning |
|----------|---------|---------|
| `HTTP_PORT` | `8080` | Cloud UI / REST port |
| `VPN_SUBNET` | `10.9.0.0/24` | OpenVPN tunnel subnet |
| `PROXY_START` / `PROXY_END` | `5001` / `6000` | Device-UI reverse-proxy port pool |
| `HTTP_WORKERS` | `4` | iot-httpd handler threads |
| `CLOUD_IMAGE` | `docker.io/naushada/iot-cloud:latest` | Image to run |
| `RESET_CONFIG` | `1` | Reload schema/config from the image on each start (see §7). `0` keeps manual `/etc/iot` edits |

Example:

```bash
HTTP_PORT=8443 VPN_SUBNET=10.8.0.0/24 ./run.sh
```

---

## 6. Day-to-day commands

```bash
./run.sh            # start (pulls image if missing)
./run.sh stop       # stop + remove containers (volumes kept)
./run.sh ps         # status
./run.sh logs       # logs + service states
./run.sh restart    # restart services
./run.sh build      # rebuild image from source
```

---

## 7. Updating to a new version

```bash
git pull                                   # latest run.sh / compose / schemas
docker pull docker.io/naushada/iot-cloud:latest   # or: ./run.sh build
./run.sh
```

On start, `run.sh` **refreshes the `iot-etc` config volume** from the image so
new data-store schemas always take effect. Without this, a named volume from a
prior run keeps the old schema and newly added keys are silently rejected
(e.g. the per-service CPU/memory telemetry would read blank). Persisted data
(`/var/lib/iot`) and the VPN PKI (`/etc/iot/vpn`, CA key) live in **separate**
volumes and are preserved across updates. Set `RESET_CONFIG=0` to opt out.

---

## 8. Persistence & volumes

| Volume | Mount | Content | Reset on start? |
|--------|-------|---------|-----------------|
| `cloud_iot-etc` | `/etc/iot` | Lua schemas + config (from image) | **Yes** (RESET_CONFIG=1) |
| `cloud_iot-lib` | `/var/lib/iot` | Persisted data store | No |
| `cloud_iot-run` | `/var/run/iot` | ds-server Unix socket | No |
| `cloud_iot-vpn` | `/etc/iot/vpn` | OpenVPN server certs / PKI | No |
| `cloud_iot-ca-key` | `/run/secrets/iot-ca-key` | CA private key (auto-generated first run) | No |

To wipe **everything** and start fresh (also drops persisted data + PKI):

```bash
./run.sh stop
docker volume rm cloud_iot-etc cloud_iot-lib cloud_iot-run cloud_iot-vpn cloud_iot-ca-key
./run.sh
```

---

## 9. Uninstall

```bash
./run.sh stop
docker volume rm cloud_iot-etc cloud_iot-lib cloud_iot-run cloud_iot-vpn cloud_iot-ca-key 2>/dev/null
docker rmi docker.io/naushada/iot-cloud:latest
```

---

## 10. Troubleshooting

- **Metrics blank for some services** — stale config volume with an old
  schema. The default `RESET_CONFIG=1` fixes this on the next `./run.sh`; or
  manually: `./run.sh stop && docker volume rm cloud_iot-etc && ./run.sh`.
  (`openvpn-server` shows blank by design — it runs inside iot-cloudd's
  container, so its usage is folded into cloudd's totals.)
- **UI not reachable** — check `./run.sh ps`, confirm port 8080 is open in any
  host firewall / cloud security group, and that `HTTP_PORT` matches.
- **`docker compose` not found** — install the Compose v2 plugin (it ships
  with Docker Desktop; on servers `apt-get install docker-compose-plugin`).
- **Permission denied talking to docker** — add your user to the `docker`
  group (`sudo usermod -aG docker $USER`) and re-login.
- **Inspect the data store directly:**
  ```bash
  docker exec iot-ds-server ds-cli --socket=/var/run/iot/data_store.sock \
    get services.cloud.iot.cloudd.state
  ```

See `apps/cloud/CLAUDE.md` for architecture and data-store key reference.
