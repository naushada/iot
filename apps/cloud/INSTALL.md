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
- **Open ports** on the host (defaults; configurable, see §6):

  | Port | Proto | Purpose |
  |------|-------|---------|
  | 80 | tcp | Cloud UI + REST API |
  | 5684 | udp | LwM2M Bootstrap (CoAPs) |
  | 5683 | udp | LwM2M Device Management (CoAPs) |
  | 1194 | tcp | OpenVPN (device tunnels, served by iot-cloudd; `cloud.vpn.proto=tcp-server`) |

  On the host you can open these in one shot with the bundled helper
  (idempotent, persists across reboot):

  ```bash
  sudo ./host-firewall.sh
  ```

  Note: on a VPS the **cloud-provider firewall** (Contabo Cloud Firewall,
  AWS security group, etc.) must allow these too — it's usually what
  blocks you, not host iptables.

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

**Minimal — no full clone.** The image is self-contained (binaries + schemas +
UI), so the host only needs `run.sh` + `docker-compose.yml`. Instead of cloning
you can fetch just those two (keep them together in one directory):

```bash
mkdir -p iot-cloud && cd iot-cloud
curl -fsSLO https://raw.githubusercontent.com/naushada/iot/main/apps/cloud/run.sh
curl -fsSLO https://raw.githubusercontent.com/naushada/iot/main/apps/cloud/docker-compose.yml
chmod +x run.sh
./run.sh
```

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
./run.sh build         # builds docker.io/naushada/iot-cloud:latest for the host arch
PULL=0 ./run.sh        # start using the local build (don't re-pull)
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

- **Cloud UI:** http://<host-ip>
- **Login:** `admin` / `admin`

The **Services** page shows each daemon's state plus live CPU % / #CPU /
Memory / FDs / Threads.

---

## 5. Production hardening

Before exposing the host to the network, lock down the defaults:

1. **Change the admin password.** Use the **Users** page in the UI, or set it
   directly (SHA-256, hashed server-side — there is no plaintext path):
   ```bash
   HASH=$(printf '%s' 'YOUR_NEW_PASSWORD' | sha256sum | cut -d' ' -f1)
   docker exec iot-ds-server ds-cli --socket=/var/run/iot/data_store.sock \
     set auth.users.admin.password.hash "\"$HASH\""
   ```
2. **Add per-operator users** (optional) on the **Users** page — each is
   `Admin` (read/write) or `Viewer` (read-only). They're stored hashed in the
   `auth.users.accounts` key; the built-in `admin` cannot be deleted.
3. **Enable HTTPS (turnkey).** Just start with `HTTPS=1`:
   ```bash
   HTTPS=1 ./run.sh
   ```
   This generates a **self-signed cert** into `./tls/` (override the cert
   subject with `TLS_HOST=<ip-or-domain>`, default = the host's primary IP),
   serves the UI over **https on 443**, and runs a small redirector on **80**
   that 301s `http://` → `https://`. Browsers warn on the self-signed cert
   for a bare IP — that's expected; click through, or drop your own
   `tls/server.crt` + `tls/server.key` in before starting to avoid it.

   Open ports change to **443/tcp** (UI) and **80/tcp** (redirect) — allow
   both in your firewall.

   *Manual / advanced:* iot-httpd terminates TLS itself, so you can also drive
   it via ds-cli (hot-reloads in ~2s) instead of `HTTPS=1`:
   ```bash
   DS(){ docker exec iot-ds-server ds-cli --socket=/var/run/iot/data_store.sock "$@"; }
   DS set http.listen.scheme '"https"'
   DS set http.tls.cert '"/etc/iot/tls/server.crt"'
   DS set http.tls.key  '"/etc/iot/tls/server.key"'
   DS set http.tls.ca   '"/etc/iot/tls/clients-ca.crt"'   # optional: enables mutual TLS
   ```
4. **Keep auth enabled** — `http.auth.enabled=true` is the default. Only disable
   it for local debugging: `DS set http.auth.enabled false`.

See `DEPLOY.md` for TLS/PKI and OpenVPN server-certificate details.

---

## 6. Configuration

### Environment variables

Override on the command line — they pass through to `docker-compose.yml`:

| Variable | Default | Meaning |
|----------|---------|---------|
| `HTTPS` | `0` | `1` = serve https on 443 (self-signed cert auto-generated) + redirect 80→443 (see §5) |
| `TLS_HOST` | host primary IP | Cert subject/SAN when `HTTPS=1` (set to your IP or domain) |
| `HTTP_PORT` | `80` (`443` if `HTTPS=1`) | Cloud UI / REST port |
| `VPN_SUBNET` | `10.9.0.0/24` | OpenVPN tunnel subnet |
| `PROXY_START` / `PROXY_END` | `5001` / `6000` | Device-UI reverse-proxy port pool |
| `HTTP_WORKERS` | `4` | iot-httpd handler threads |
| `CLOUD_IMAGE` | `docker.io/naushada/iot-cloud:latest` | Image to run |
| `PULL` | `1` | Pull the image on start. `0` = use a local `./run.sh build` as-is |
| `PLATFORM` | auto (`uname -m`) | Image arch to pull. The published image is multi-arch (`linux/amd64` + `linux/arm64`), so the host arch is selected automatically; override e.g. `linux/amd64` |
| `RESET_CONFIG` | `1` | Reload schema/config from the image on each start (see §8). `0` keeps manual `/etc/iot` edits |

Example:

```bash
HTTP_PORT=8443 VPN_SUBNET=10.8.0.0/24 ./run.sh
```

### LwM2M server URIs — devices must reach these

By default the bootstrap and DM servers advertise themselves as
`coaps://0.0.0.0:5684` / `coaps://0.0.0.0:5683`. That works for a
local smoke test, but a real device bootstraps fine and then tries to
register against `0.0.0.0` and fails. Set both to **this host's public
IP or DNS name** so the URI handed to devices is reachable:

```bash
DS(){ docker exec iot-ds-server ds-cli --socket=/var/run/iot/data_store.sock "$@"; }
DS set cloud.bs.uri '"coaps://217.217.253.235:5684"'   # bootstrap entry point
DS set cloud.dm.uri '"coaps://217.217.253.235:5683"'   # DM URI pushed to devices (Server Object)
```

`cloud.dm.uri` is the one that bites: it's stamped into each device's
Server Object during bootstrap, so if it stays `0.0.0.0` the device
bootstraps but can never find the DM server. Both are also editable in
the UI under **LwM2M → Bootstrap Config**; changes hot-reload.

On the device side, point it at this host's bootstrap server — see
[`DEPLOY.md`](../../DEPLOY.md) Path C, step 4.

---

## 7. Day-to-day commands

```bash
./run.sh            # start (pulls image if missing)
./run.sh stop       # stop + remove containers (volumes kept)
./run.sh ps         # status
./run.sh logs       # logs + service states
./run.sh restart    # restart services
./run.sh build      # rebuild image from source
```

---

## 8. Updating to a new version

```bash
git pull && ./run.sh
```

`git pull` updates `run.sh` / compose / schemas; `run.sh` then pulls the
latest image (`PULL=1` by default) and recreates the containers — no manual
`docker pull`, stop, or `docker rmi` needed. To use a locally-built image
instead, run `./run.sh build` first and start with `PULL=0 ./run.sh`.

On start, `run.sh` **refreshes the `iot-etc` config volume** from the image so
new data-store schemas always take effect. Without this, a named volume from a
prior run keeps the old schema and newly added keys are silently rejected
(e.g. the per-service CPU/memory telemetry would read blank). Persisted data
(`/var/lib/iot`) and the VPN PKI (`/etc/iot/vpn`, CA key) live in **separate**
volumes and are preserved across updates. Set `RESET_CONFIG=0` to opt out.

---

## 9. Persistence & volumes

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

## 10. Uninstall

```bash
./run.sh stop
docker volume rm cloud_iot-etc cloud_iot-lib cloud_iot-run cloud_iot-vpn cloud_iot-ca-key 2>/dev/null
docker rmi docker.io/naushada/iot-cloud:latest
```

---

## 11. Troubleshooting

- **Metrics blank for some services** — stale config volume with an old
  schema. The default `RESET_CONFIG=1` fixes this on the next `./run.sh`; or
  manually: `./run.sh stop && docker volume rm cloud_iot-etc && ./run.sh`.
  (`openvpn-server` shows blank by design — it runs inside iot-cloudd's
  container, so its usage is folded into cloudd's totals.)
- **UI not reachable** — check `./run.sh ps`, confirm port 80 is open in any
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
