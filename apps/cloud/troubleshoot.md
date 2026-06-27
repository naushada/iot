# IoT Cloud — Troubleshooting

Practical recipes for the failures we actually hit bringing the cloud up
and connecting a device. Run these on the **cloud host**.

## The `DS` helper

Almost everything below pokes the data store via `ds-cli` inside the
`iot-ds-server` container. Define this once per shell:

```bash
DS(){ docker exec iot-ds-server ds-cli --socket=/var/run/iot/data_store.sock "$@"; }
# read:   DS get <key> [<key> ...]
# write:  DS set <key> '"<string-value>"'    # JSON-quote strings
# bool:   DS set <key> true | false
```

On the **device** side the equivalent is `./run.sh ds …` from `apps/device/`
(e.g. `./run.sh ds get iot.endpoint`).

---

## Device won't bootstrap — DTLS handshake fails

Symptom (cloud `lwm2m-bs` / `log.lwm2m.bs.text`):
```
dtlsGetPskInfoCb ... id_len:32 ... PSK for unknown identity requested
no psk key for session available
```
…and on the device, `Alert: level 2, description 47` → `removed peer`.

**Meaning:** the BS has no PSK credential for the identity the device sent.
The BS identity is **derived**: `sha256(endpoint)[:32]` on both sides — you
never type it. So this is almost always **"not provisioned"** (or provisioned
under a different serial).

### Fix — provision the endpoint

The credential is only stored when the **BS PSK carrier is set** before the
request (the `if (!bs_psk.empty())` path). Order matters:

```bash
# 1. get the device's endpoint + BS PSK (32 hex = 16 bytes):
#    apps/device $  ./run.sh ds get iot.endpoint iot.bs.psk.key

# 2. on the cloud:
DS set cloud.dev.mode true
DS set cloud.provision.bs.psk '"<device iot.bs.psk.key, 32 hex>"'   # the carrier — easy to forget
DS set cloud.provision.request '"<device iot.endpoint, e.g. urn:dev:device-1>"'

# 3. confirm — cloudd logs:  stored credentials for <ep> (identity=rpi<ep>@cloud.local)
DS get cloud.endpoint.credentials          # must contain bs.psk.key for the serial
```

Notes:
- **PSK must be 32 hex / 16 bytes.** tinydtls' `TLS_PSK_WITH_AES_128_CCM_8`
  key buffer is 16 bytes; a 64-hex key overflows it and the handshake fails.
- The **serial you provision must equal the device's `iot.endpoint`** —
  `sha256()` of it must match the `id_len:32` the device presents.
- `cloud.provision.request` **upserts** — re-running replaces the entry, so
  you don't need to remove first.

### Verify the identity lines up

```bash
printf '%s' 'urn:dev:device-1' | sha256sum | cut -c1-32   # = the id_len:32 in the device log
```

### Device sends its formatted identity at BS (`rpi<serial>@cloud.local`)

Symptom (cloud `lwm2m-bs`):
```
PSK resolver: no key for BS identity 'rpi000000006556e041@cloud.local' — not in cloud.endpoint.credentials (HKDF tier off)
dtls: PSK for unknown identity requested
```
i.e. the presented identity is the **DM-style** `rpi<serial>@cloud.local`, not
the canonical `sha256(serial)[:32]`. That happens when the device has
`iot.bs.psk.override=true` with `iot.bs.psk.identity=rpi<serial>@cloud.local`.

**Why it often appears after a cloud reboot:** the DM registry is in-memory, so
a reboot drops every registration. Registered devices then fall back to
**re-bootstrap**, which exposes this latent identity mismatch — devices that
were happily registered go offline and loop in `bootstrapping`.

The BS resolver now **accepts both** forms: it tries `sha256(serial)[:32]`
first, then falls back to matching a `cloud.endpoint.credentials` row's
`identity` / `dm.psk.id`, returning the same `bs.psk.key`. So on an up-to-date
cloud image this resolves itself. If you see it, the `lwm2m-bs` container is
running an **older image** — rebuild + redeploy it.

**Immediate field recovery (no cloud rebuild)** — drop the override on the
device so it presents the canonical identity (the device's `iot.bs.psk.key`
must equal the cloud row's `bs.psk.key`):
```bash
# on the device:
ds-cli get iot.bs.psk.key                 # confirm == cloud row's bs.psk.key
ds-cli set iot.bs.psk.override false
systemctl restart iot-lwm2m-client
# device now presents sha256(serial)[:32] → cloud matches → registers
```

---

## Cloud advertises an unreachable DM (bootstrap OK, register fails)

The BS pushes `cloud.dm.uri` to the device during `/bs`. If it's still the
default `coaps://0.0.0.0:5683`, the device bootstraps but can't find the DM:

```bash
DS get cloud.dm.uri cloud.bs.uri
DS set cloud.dm.uri '"coaps://217.217.253.235:5683"'   # public IP, not 0.0.0.0
DS set cloud.bs.uri '"coaps://217.217.253.235:5684"'
```

---

## `lwm2m-dm bind failed port:5683 errno:98`

`errno 98` = address already in use — usually a leftover from a daemon/compose
restart where two DM instances briefly overlapped. The DM "self-reports
running" but isn't actually listening. Clean restart:

```bash
docker restart iot-lwm2m-dm
docker logs iot-lwm2m-dm --tail 8        # should listen on 5683, NO "bind failed"
```

---

## Firewall — device traffic dropped

The device-facing LwM2M ports are **UDP** (easy to add as TCP by mistake):

| Proto | Port | Purpose |
|-------|------|---------|
| TCP | 80 / 443 | Cloud UI (http / https) |
| TCP | 22 | SSH |
| **UDP** | **5684** | LwM2M Bootstrap |
| **UDP** | **5683** | LwM2M Device Management |
| TCP | 1194 | OpenVPN (per `cloud.vpn.proto`) |

On a VPS the **provider firewall** (Contabo Cloud Firewall, AWS SG…) must
allow these too — it's usually what silently drops them. Host side: see
`host-firewall.sh`. Quick reachability check from elsewhere:
`nc -vzu <host> 5684` (UDP), `nc -vz <host> 443` (TCP).

**timeout vs refused — read it right.** A `nc` / SSH **timeout** means the
packet was silently *dropped* → a firewall is filtering that port. A
**"Connection refused"** (TCP RST) means the packet *reached* the host and
nothing's listening → host is reachable, firewall is NOT the problem. So
"22 times out but 80 refuses" = a firewall rule on 22 specifically, not a
dead host. UDP gives neither: a dropped UDP probe and an open-but-idle UDP
port both look identical (`nc -u` prints "succeeded"), so confirm UDP from
the host with `ss -ulnp | grep -E '5683|5684'`, not from outside.

### Contabo Cloud Firewall is **default-deny** — allow, don't remove

The Contabo Cloud Firewall is an **allow-list**: while it's attached to the
VPS, *everything* is dropped except ports you explicitly allow. Two traps we
hit:

- **Editing it resets the whole policy.** Deleting "a rule" doesn't open a
  port — it removes an *allow* and tightens the box (we watched port 80 flip
  from open → filtered after a delete). To open a port you **add an inbound
  allow rule**, you don't remove rules.
- **Protocol defaults to TCP.** 5683 / 5684 / 1194 must be set to **UDP**
  explicitly, or CoAP/DTLS/OpenVPN silently never arrive.

Build **one** correct inbound allow-list and leave it alone:

| Proto | Port | Purpose |
|-------|------|---------|
| TCP | 22 | SSH (scope to your IP) |
| TCP | 80 / 443 | Cloud UI |
| UDP | 5684 | LwM2M Bootstrap |
| UDP | 5683 | LwM2M Device Management |
| UDP *(or TCP)* | 1194 | OpenVPN — match `cloud.vpn.proto` |

Fastest unblock if you're fighting the panel: **detach the Cloud Firewall**
from the VPS entirely. If host `ufw` is inactive the box becomes fully
reachable again, then secure it at the OS level with `host-firewall.sh` /
`ufw` instead. Recover SSH at any time via the Contabo **VNC / noVNC
console** in the panel — it bypasses SSH and the firewall.

---

## SSH — `kex_exchange_identification: Connection reset by peer`

The TCP connection to port 22 **succeeds** (firewall is open) but sshd
resets the session mid-handshake. This is **not** a firewall drop — the host
is actively refusing the login. Most common cause on a hardened VPS:
**TCP wrappers** denying you via `/etc/hosts.deny`.

```bash
sudo systemctl status ssh --no-pager      # look for: refused connect from <your-ip>
cat /etc/hosts.deny                        # the smoking gun, e.g.  sshd: ALL
cat /etc/hosts.allow
```

`sshd: ALL` in `/etc/hosts.deny` = deny SSH from everyone except whoever is
whitelisted in `/etc/hosts.allow` (a `DenyHosts`-style or provider default).
sshd accepts the TCP connection then immediately closes it → the
`kex_exchange_identification` reset. `hosts.allow` is consulted **before**
`hosts.deny`, so whitelist your IP — it wins, no sshd restart needed:

```bash
echo 'sshd: <your-public-ipv4>' | sudo tee -a /etc/hosts.allow   # keeps deny-by-default
# …or open SSH to all (firewall still applies):
sudo sed -i '/^sshd:[[:space:]]*ALL/d' /etc/hosts.deny
```

Find your public **IPv4** (the one sshd sees) with `curl -4 ifconfig.me`.
If `DenyHosts` is installed it'll re-ban you after repeated probing — the
`hosts.allow` whitelist is what makes it stick. Other resets-at-kex causes
if `hosts.deny` is clean: missing host keys (`sudo ssh-keygen -A`), missing
`/run/sshd`, or a bad `sshd_config` (`sudo sshd -t`). The journal names it:
`sudo journalctl -u ssh -n 50 --no-pager`.

> Run these from the Contabo **VNC console** when SSH is the thing that's
> locked out.

---

## Image pull fails / stalls — `127.0.0.53:53 i/o timeout`, EOF, stuck

Symptoms during `./run.sh`:

```
lookup registry-1.docker.io on 127.0.0.53:53: read udp ...->127.0.0.53:53: i/o timeout
failed to copy: httpReadSeeker: failed open: ... EOF      # blob drops mid-download
[+] up 8/25 ... Interrupted                                # pull stuck
```

Two compounding causes on a fresh VPS:

1. **Broken/blocked DNS.** The host resolver (`127.0.0.53`, systemd-resolved)
   times out — usually because **outbound UDP 53 is blocked** by the Cloud
   Firewall's egress policy. Test: `ping -c2 8.8.8.8` works but
   `dig +short registry-1.docker.io @8.8.8.8` times out ⇒ egress UDP 53 is
   filtered. Fix: allow **outbound UDP 53 + TCP 53** (and outbound TCP 443/80
   for the pull itself) in the Cloud Firewall — or, simplest, leave the
   **egress policy allow-all** and only filter inbound.
2. **Lossy link.** Parallel blob fetches saturate a packet-lossy VPS link and
   time each other out (the `EOF` mid-blob).

### Fix in place (point the engine at public DNS + serialize)

**Docker** — `dockerd` does the pull, so configure the daemon:
```bash
sudo tee /etc/docker/daemon.json >/dev/null <<'EOF'
{ "dns": ["8.8.8.8","1.1.1.1"], "max-concurrent-downloads": 1, "max-download-attempts": 10 }
EOF
sudo systemctl restart docker
docker pull docker.io/naushada/iot-cloud:latest    # resumable — re-run until clean
```

**Podman** — the `podman` process resolves via the **host** libc resolver
(no daemon), so fix DNS at the host and use pull retries:
```bash
sudo mkdir -p /etc/systemd/resolved.conf.d
printf '[Resolve]\nDNS=8.8.8.8 1.1.1.1\nFallbackDNS=9.9.9.9\n' \
  | sudo tee /etc/systemd/resolved.conf.d/dns.conf
sudo systemctl restart systemd-resolved
podman pull --retry 10 --retry-delay 5s docker.io/naushada/iot-cloud:latest
```

Both `docker pull` and `podman pull` are **resumable** — completed layers are
cached, so just re-run until it finishes.

### Out-of-band transfer (when the link is too lossy to pull at all)

Pull/build on a machine with good connectivity, ship the image as a file, and
the VPS never touches the registry. `run.sh` uses the local image if present.

⚠️ **Arch:** the VPS is **linux/amd64**. Building/pulling on an Apple-Silicon
Mac yields **arm64**, which won't run there — force `--platform linux/amd64`.

**Docker:**
```bash
# Good-network machine (e.g. your Mac):
docker pull --platform linux/amd64 docker.io/naushada/iot-cloud:latest
#   or build it: docker build --platform linux/amd64 \
#                  -t naushada/iot-cloud:latest -f apps/cloud/Dockerfile .
docker save docker.io/naushada/iot-cloud:latest | gzip > iot-cloud.tar.gz
scp iot-cloud.tar.gz engineer@<host>:~/

# VPS:
gunzip -c iot-cloud.tar.gz | docker load
HTTPS=1 ./run.sh                                   # starts from local image, no pull
```

**Podman** (same `save`/`load` syntax):
```bash
# Good-network machine:
podman pull --platform linux/amd64 docker.io/naushada/iot-cloud:latest
podman save docker.io/naushada/iot-cloud:latest | gzip > iot-cloud.tar.gz
scp iot-cloud.tar.gz engineer@<host>:~/

# VPS:
gunzip -c iot-cloud.tar.gz | podman load
HTTPS=1 ./run.sh
```

Or skip the registry entirely with the **build-locally path** (Path B in
`INSTALL.md`) if the box has the source tree.

---

## HTTPS — page won't open on 443

```bash
docker logs iot-httpd --tail 10
```
- `TLS init failed: certificate … No such file or directory` → no cert. The
  image self-provisions one at startup (entrypoint + image `openssl`) into the
  `iot-tls` volume; if you're on an older image, generate it once:
  ```bash
  docker run --rm --entrypoint openssl -v "$PWD/tls":/tls docker.io/alpine/openssl \
    req -x509 -newkey rsa:2048 -nodes -days 825 \
    -keyout /tls/server.key -out /tls/server.crt \
    -subj "/CN=<host-ip>" -addext "subjectAltName=IP:<host-ip>"
  docker restart iot-httpd
  ```
- `443 Connection refused` → no https server running; you didn't start with
  `HTTPS=1`. Redeploy: `HTTPS=1 ./run.sh`.
- Spammy `TLS handshake failed` on 443 from random IPs → internet bots probing
  the public port. Harmless noise.

---

## UI pages slow / blank

If config pages (LwM2M, Logs, Users) hang or render only the heading, the
httpd worker pool is wedged. Historically a root-path `/` request looped a
worker forever (fixed in current images). Quick unblock:

```bash
docker restart iot-httpd
```
Then pull a current image (`git pull && ./run.sh`), which has the loop fix +
the long-poll-on-navigation cleanup.

---

## Stale config volume after an update

`run.sh` refreshes the `iot-etc` config volume each start (`RESET_CONFIG=1`)
so new schemas take effect. If a newly-added ds key reads blank after an
update, you likely started with `RESET_CONFIG=0`; drop the volume:

```bash
./run.sh stop
docker volume rm cloud_iot-etc
./run.sh
```

## Redeploys leave orphan containers / `rmi` says "image is being used"

Symptoms after pulling a new image:

```
docker rmi -f <id>   → conflict: image is being used by running container <c>
./run.sh stop        → ! Network cloud_default Resource is still in use
```

Cause: `docker compose down` only stops the services in the **current**
compose file. When the service set changes between releases (e.g. a service
is renamed/removed, or a `ports:` / networking change between releases),
the old container becomes an **orphan** — still `Up` (often
`restart: unless-stopped`), still attached to `cloud_default`, still holding
the old image. `down` skips it, so the network can't be removed and the image
stays in use. (Same redeploy-staleness family as the bootstrap provision-watch
going stale after a redeploy — `docker restart iot-cloudd`.)

Fix — tear down **with orphan removal**, then pull + up:

```bash
docker compose down --remove-orphans     # or: docker rm -f <orphan-name>
docker rmi -f naushada/iot-cloud:latest  # now free
docker compose pull
./run.sh
```

Make `--remove-orphans` the default teardown whenever the service set changes.
