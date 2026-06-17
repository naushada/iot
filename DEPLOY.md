# Deploying iot

Three paths, your choice:

| Path            | When                                                                                      | Time-to-first-`get`    |
|-----------------|-------------------------------------------------------------------------------------------|------------------------|
| **Container**   | Cloud, edge devices that already run a container engine, local hacking                  | 5 min                  |
| **Bare metal**  | Single-purpose appliances, embedded Linux without container runtime                     | 15 min                 |
| **Yocto**       | Custom embedded Linux images (full bootable RPi 3B SD card + `.ipk` feed), cross-compilation | 2–4 hours (first build)|

After install both paths boot `ds-server` (the typed-value config plane)
plus one of the `lwm2m` roles (client or server). Live config flows
through `ds-cli` — no service restarts needed for endpoint, lifetime,
or DM server URI changes.

For the wire spec, application API, and ds-cli cookbook see:

- [modules/data-store/docs/protocol.md](modules/data-store/docs/protocol.md) — EMP framing
- [modules/data-store/docs/client_api.md](modules/data-store/docs/client_api.md) — `libdatastore_client` API
- [packaging/README.md](packaging/README.md) — build/install internals + image size budget

---

## Path A — Container (recommended for first run)

### 1. Build the runtime image

```sh
podman build -f packaging/Containerfile -t iot:l11 .
```

(~5 min cold, < 30 s warm thanks to layer caching. Final image
~119 MB; see [`packaging/README.md`](packaging/README.md) for the
size budget routes if you need it smaller.)

### 2. Start ds-server + lwm2m client

The two containers share `/run/iot` via a named volume so the lwm2m
client can connect to ds-server's unix socket:

```sh
podman volume create iot-run
podman run -d --name iot-ds     -e IOT_ROLE=ds     -v iot-run:/run/iot iot:l11
podman run -d --name iot-client -e IOT_ROLE=client -v iot-run:/run/iot iot:l11
# Optional: net-router for nftables DNAT into the local lwm2m client.
# --network=host so the daemon sees the host's ifaces + applies nft
# rules in the host's network namespace.
podman run -d --name iot-net --cap-add=NET_ADMIN --network=host \
    -e IOT_ROLE=net -v iot-run:/run/iot iot:l11
```

For the server role (Bootstrap + DM), substitute `IOT_ROLE=server` and
expose UDP ports:

```sh
podman run -d --name iot-server -e IOT_ROLE=server \
    -v iot-run:/run/iot \
    -p 5683:5683/udp -p 5685:5685/udp \
    iot:l11
```

For **openvpn-client** (L12) — needs `CAP_NET_ADMIN` and
`/dev/net/tun` because the spawned `openvpn(8)` brings up the TUN
device + writes routes:

```sh
# vpn.* keys must be set before openvpn-client starts; see modules/openvpn/client/docs/design.md
podman exec iot-ds ds-cli --socket=/run/iot/data_store.sock set vpn.remote.host '"vpn.example.com"'
podman exec iot-ds ds-cli --socket=/run/iot/data_store.sock set vpn.cert.path  '"/etc/iot/vpn/client.crt"'
podman exec iot-ds ds-cli --socket=/run/iot/data_store.sock set vpn.key.path   '"/etc/iot/vpn/client.key"'
podman exec iot-ds ds-cli --socket=/run/iot/data_store.sock set vpn.ca.path    '"/etc/iot/vpn/ca.crt"'

podman run -d --name iot-ovpn \
    --cap-add=NET_ADMIN --device=/dev/net/tun \
    --volumes-from iot-ds \
    -v /etc/iot/vpn:/etc/iot/vpn:ro \
    iot:l11 openvpn-client --ds-sock=/run/iot/data_store.sock
```

The image ships `openvpn(8)` from Ubuntu (`apt install openvpn` in
the runtime stage). Default path `/usr/sbin/openvpn`; override with
`--openvpn=PATH`.

**WAN gate (L15).** openvpn-client only spawns openvpn while
`net.iface.active` is non-empty — i.e. while net-router has
selected a usable WAN iface (eth0 / wlan0 / wwan0). In the
container path that means start the `iot-netr` container alongside
`iot-ovpn`. For dev runs without net-router, short-circuit the
gate by setting the key by hand:

```sh
podman exec iot-ds ds-cli --socket=/run/iot/data_store.sock \
    set net.iface.active '"eth0"'
```

Observe the gate via two new write keys:

```sh
podman exec iot-ds ds-cli --socket=/run/iot/data_store.sock \
    get vpn.state vpn.gate.reason vpn.bound.iface
# vpn.state       = "connected"
# vpn.gate.reason = "ok"           # "wan_down" when gated
# vpn.bound.iface = "eth0"
```

### 3. Tune config via ds-cli

```sh
podman exec iot-ds ds-cli --socket=/run/iot/data_store.sock \
    set iot.endpoint '"urn:dev:my-device-1"'

podman exec iot-ds ds-cli --socket=/run/iot/data_store.sock \
    set iot.lifetime 600

podman exec iot-ds ds-cli --socket=/run/iot/data_store.sock \
    get iot.endpoint iot.lifetime
```

The lwm2m container picks these up via its `DsConfig` watch +
on_change handlers (FUP-DS-9..11). Watch the log:

```sh
podman logs -f iot-client | grep "applied iot"
# applied iot.endpoint=urn:dev:my-device-1 — re-register cycle queued ...
# applied iot.lifetime=600 to live RegistrationClient — next Update tick uses it
```

### 4. Verify the full pipeline

Run the end-to-end smoke that this repo ships:

```sh
sh log/L11/e2e-smoke.sh
```

It builds the image, starts both containers, mutates a key, asserts
the hot-apply landed, and confirms schema rejection still gates. The
last run is captured at [`log/L11/e2e-smoke.txt`](log/L11/e2e-smoke.txt).

---

## Path C — Yocto / OpenEmbedded (full distribution for Raspberry Pi 3B)

This path builds a **complete bootable Linux distribution** — the
`iot-image` — cross-compiled inside a podman/docker container. The host
needs only podman or docker; no Yocto SDK, no host Yocto install. The
build runs entirely in the container and the artifacts are copied back
to the host.

Two artifacts come out per machine, copied back to the host under
`yocto/build/<machine>/`:

- **`images/<machine>/iot-image-<machine>.rootfs-<timestamp>.wic.bz2`** — a
  flashable SD-card image. The default `MACHINE` is `raspberrypi3-64`
  (Pi 3B, 64-bit), so the Pi image lands at
  `yocto/build/raspberrypi3-64/images/raspberrypi3-64/`. A stable
  `iot-image-<machine>.rootfs.wic.bz2` symlink in the same directory always
  points at the latest build. It bundles the full gateway stack
  (`packagegroup-iot-full`), all kernel modules, the Pi's onboard
  Wi-Fi/Bluetooth firmware, an SSH server, and `opkg`.
- **`ipk/` feed** — the same per-daemon packages, for pushing app
  updates to an already-running target over ssh.

> First boot needs a physical SD-card flash — a Pi can't be
> bootstrapped purely over ssh. Once it's booted with sshd, all
> subsequent app updates go over ssh via the `.ipk` feed.

### 1. Build the image

```sh
cd yocto
./build.sh                           # raspberrypi3-64 image (default)
MACHINE=qemuarm64 ./build.sh         # ARM64 qemu image
TARGET=packagegroup-iot ./build.sh   # just the .ipk feed, skip the image
./build.sh all                       # every machine in build.sh
```

The script builds the container image (`iot-yocto-builder`, defined by
`yocto/Containerfile`), runs `bitbake iot-image`, and copies the output
to `yocto/build/<machine>/` (`images/<machine>/*.wic.bz2` + `ipk/`).

The **first** build compiles the whole distribution (toolchain, glibc,
ACE, kernel…) — hours. Reruns are incremental via the persistent
`iot-yocto-sstate` / `iot-yocto-downloads` volumes (only `meta-iot`
recompiles). To skip the distro compile on another machine / CI, publish a
cache image once and consume it elsewhere:

```sh
podman login docker.io
CACHE_IMAGE=docker.io/<you>/iot-yocto-builder:cache ./build.sh publish        # after a full build
CACHE_IMAGE=docker.io/<you>/iot-yocto-builder:cache IOT_USE_CACHE=1 ./build.sh # elsewhere → only meta-iot rebuilds
```

See [`yocto/meta-iot/README.md`](yocto/meta-iot/README.md) for the full
build-steps walkthrough.

### 2. Flash the SD card

**Easiest — the helper script.** `yocto/flash-sd.sh` auto-detects the SD
card, wipes its partition table, and writes the latest image. It refuses
internal/system disks and asks for confirmation first:

```sh
cd yocto
./flash-sd.sh                 # auto-detect the card (errors if 0 or >1 found)
./flash-sd.sh /dev/sdX        # or name the device explicitly
./flash-sd.sh --list          # just list candidate removable disks
./flash-sd.sh --yes /dev/sdX  # non-interactive (skip the confirmation)
```

It picks `MACHINE=raspberrypi3-64` by default; override with `MACHINE=…` or
point at any image with `IMAGE=/path/to.wic.bz2`.

**Manual.** Find the SD device first — `lsblk` on Linux (`/dev/sdX`),
`diskutil list` on macOS (`/dev/diskN`; unmount with `diskutil unmountDisk`
first). Write the **whole disk**, not a partition. Decompress on the fly and
write with `dd`:

```sh
IMG=yocto/build/raspberrypi3-64/images/raspberrypi3-64/iot-image-raspberrypi3-64.rootfs.wic.bz2

bzcat $IMG | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
# macOS: target the raw node /dev/rdiskN instead of /dev/diskN — much faster.
```

> **Pick the right disk — `dd` is irreversible and writes the whole device.**
> On macOS run `diskutil list` and identify the card by **size** (e.g. a 32 GB
> card), *not* the internal Mac disk (`disk0`, usually the largest with an
> `APFS`/`Apple_APFS` container). Writing to the wrong `/dev/diskN` destroys
> that disk with no undo. Full macOS sequence:
>
> ```sh
> diskutil list                                   # find the card by size
> diskutil unmountDisk /dev/diskN                 # unmount (don't eject yet)
> bzcat $IMG | sudo dd of=/dev/rdiskN bs=4m       # raw node = much faster
> sync                                            # flush write cache
> diskutil eject /dev/diskN                       # safe to remove
> ```
>
> (`bs=4m` is the BSD `dd` spelling on macOS; `bs=4M` is the GNU/Linux one.)

> The build does **not** emit a `.bmap` file: `bmaptool` needs the FIEMAP
> ioctl / `SEEK_HOLE`, which the macOS-podman build filesystem doesn't
> support, so `wic.bmap` is omitted from `IMAGE_FSTYPES`. If you build on a
> Linux host and want `bmaptool copy`'s faster verified writes, add
> `"wic.bmap"` back to `IMAGE_FSTYPES` in `meta-iot/recipes-iot/images/iot-image.bb`.

### 3. Boot + ssh in

Insert the card, power the Pi (ethernet is simplest for first bring-up;
a serial console is on the GPIO UART since `ENABLE_UART=1`). The image
runs `sshd`; `debug-tweaks` leaves the root password empty for the first
login:

```sh
ssh root@<pi-ip>
# Production hardening: set a password / provision an ssh key, then
# rebuild without debug-tweaks (EXTRA_IMAGE_FEATURES is overridable).
```

`ds-server` and the full gateway stack are already installed and managed
by systemd. Only `iot-ds.service` auto-starts; enable the role daemons
you need (see Path B for the per-daemon ds-cli seeding):

```sh
systemctl enable --now iot-lwm2m-client iot-httpd
ds-cli --socket=/run/iot/data_store.sock get iot.endpoint
```

### 4. Point the device at your cloud

The device only needs the **bootstrap** server address — the DM server
URI and credentials come down during bootstrap. Set it live via ds-cli;
it hot-reloads into the running client, no reflash:

```sh
ds-cli --socket=/run/iot/data_store.sock set iot.bs.uri '"coaps://217.217.253.235:5684"'
ds-cli --socket=/run/iot/data_store.sock get iot.bs.uri
```

Registration also needs the bootstrap PSK to match the cloud's
`cloud.bs.psk.*` (the commissioning step — see `apps/cloud/CLAUDE.md`).
If these are empty the DTLS handshake to the BS server fails and the
client sits in `AwaitingRegisterAck`:

```sh
ds-cli --socket=/run/iot/data_store.sock get iot.endpoint        # endpoint name (auto-filled from serial on RPi)
ds-cli --socket=/run/iot/data_store.sock get iot.bs.psk.identity # must match cloud cloud.bs.psk.id
ds-cli --socket=/run/iot/data_store.sock get iot.bs.psk.key      # 64-hex PSK, must match cloud cloud.bs.psk.key
```

Watch the full flow — bootstrap finish → register to DM → registration ACK:

```sh
journalctl -u iot-lwm2m-client -f
# look for an ACK from 217.217.253.235:5683 (the DM server)
```

> The cloud must advertise a **reachable** DM URI (`cloud.dm.uri`), not the
> default `coaps://0.0.0.0:5683`, or the device bootstraps but can't find
> the DM server afterwards. See [`apps/cloud/INSTALL.md`](apps/cloud/INSTALL.md) §6.

### 5. Push app updates over ssh (the `.ipk` feed)

After the device is up, ship new daemon builds without reflashing —
`opkg` and the feed config are baked into the image:

```sh
scp yocto/build/raspberrypi3-64/ipk/*/iot-*.ipk root@<pi-ip>:/tmp/
ssh root@<pi-ip> 'opkg install /tmp/iot-*.ipk && systemctl restart iot-httpd'
```

Full Yocto / layer docs, including how to change the architecture, the
image payload, or the Wi-Fi firmware package:
[`yocto/meta-iot/README.md`](yocto/meta-iot/README.md).

---

## Path B — Bare metal / VM

### 1. Build + install

```sh
# Dependencies (Ubuntu 22.04 reference)
sudo apt install -y cmake g++ libssl-dev zlib1g-dev libreadline-dev \
                    liblua5.3-dev libace-dev libmongocxx-dev libbsoncxx-dev

cmake -B build -DCMAKE_INSTALL_PREFIX=/usr
make -C build -j"$(nproc)"
sudo make -C build install
```

Layout (under `--prefix=/usr`):

```
/usr/bin/{ds-server, ds-cli, lwm2m}
/usr/lib/<triplet>/libdatastore_client.a
/usr/lib/systemd/system/iot-{ds, lwm2m-client, lwm2m-server}.service
/usr/lib/tmpfiles.d/iot.conf
/etc/iot/ds-schemas/iot.lua
/etc/iot/{lwm2m-client, lwm2m-server}.env
/etc/iot/config/<object>/*.lua.example
```

### 2. Copy + edit your config templates

The packaged `.lua.example` files exist so `sudo apt upgrade` (or your
equivalent) never silently clobbers your real config. Copy one to
`.lua` and edit:

```sh
cd /etc/iot/config/securityObject
sudo cp 0.lua.example 0.lua
sudoedit 0.lua             # set serverUri, identity, secretKey
```

Repeat for `serverObject/0.lua`, `deviceObject/0.lua` as needed for
your role.

### 3. Enable + start

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now iot-ds.service iot-lwm2m-client.service
sudo systemctl status iot-ds iot-lwm2m-client
```

The lwm2m-client unit `After=iot-ds.service Wants=iot-ds.service` so
DsConfig connects on the first try without falling back to defaults.

For the **openvpn-client** unit, seed the required vpn.* keys first
(or it will refuse to start) and then enable:

```sh
ds-cli --socket=/run/iot/data_store.sock set vpn.remote.host '"vpn.example.com"'
ds-cli --socket=/run/iot/data_store.sock set vpn.cert.path  '"/etc/iot/vpn/client.crt"'
ds-cli --socket=/run/iot/data_store.sock set vpn.key.path   '"/etc/iot/vpn/client.key"'
ds-cli --socket=/run/iot/data_store.sock set vpn.ca.path    '"/etc/iot/vpn/ca.crt"'

sudo systemctl enable --now iot-openvpn-client.service
journalctl -u iot-openvpn-client.service -f | grep "PUSH_REPLY\|state\|WAN"
```

The unit ships with `AmbientCapabilities=CAP_NET_ADMIN` +
`DeviceAllow=/dev/net/tun rw` so openvpn(8) can create the TUN
device + write routes under DynamicUser — no `root` required.

**WAN gate.** openvpn-client now subscribes to `net.iface.active`
and only spawns openvpn while a WAN iface is up. Enable
`iot-net-router.service` alongside (or set the key by hand for
dev). Reason exposed via `vpn.gate.reason` (`ok` while running,
`wan_down` while gated, `spawn_failed` after a spawn error);
bound iface in `vpn.bound.iface`. See
[`modules/openvpn/client/docs/design.md`](modules/openvpn/client/docs/design.md)
§ "WAN gate" for the state machine.

For the **net-router** unit (L13: nftables DNAT + iface priority),
just enable it — **`net.lwm2m.target.ip` is optional**:

```sh
# Optional: a DNAT target only matters for a GATEWAY that forwards inbound
# LwM2M to a DOWNSTREAM device. A device that is its OWN LwM2M client needs
# none — leave it unset and net-router still runs.
ds-cli --socket=/run/iot/data_store.sock set net.lwm2m.target.ip '"192.168.10.5"'  # gateway topology only
# Optional: rename per-class iface keys to match your host
ds-cli --socket=/run/iot/data_store.sock set net.iface.eth.name      '"eth0"'
ds-cli --socket=/run/iot/data_store.sock set net.iface.wifi.name     '"wlan0"'
ds-cli --socket=/run/iot/data_store.sock set net.iface.cellular.name '"wwan0"'

sudo systemctl enable --now iot-net-router.service
journalctl -u iot-net-router.service -f | grep "ruleset applied\|metric\|active"
```

net-router starts on boot regardless of config: it elects the
highest-priority OPER-up WAN iface (publishes `net.iface.active`),
installs the base firewall + route metrics, and publishes
`services.net.router.state="running"` so dependent services
(`openvpn-client`, `lwm2m-client`) unpark. Its lifecycle state
(`net.state`) tracks the uplink — `need-config` while no usable WAN
iface is present, `steady` once one is up. The DNAT/port-forward rules
are installed **only** when `net.lwm2m.target.ip` is set (default
forwarded ports `80,443,5684`, HTTP/HTTPS + LwM2M/CoAP-over-DTLS,
overridable via `net.forward.ports`); set them in the device-UI under
**Routing → DNAT Target / Port Forward**, or via the keys directly. Add
operator drops/forwards via the JSON `net.custom.rules` schema key (see
`/etc/iot/ds-schemas/net.lua`). The unit ships with
`AmbientCapabilities=CAP_NET_ADMIN` — enough for `nft -f` and
`ip route replace` under DynamicUser.

For the **wifi-client** unit (L15: wpa_supplicant + DHCP-client
supervisor; publishes `wifi.*`), seed the network list and enable.
NetworkManager MUST be masked on hosts running wifi-client — the
daemon's startup probe writes `wifi.assoc.state="conflict"` and
refuses to spawn wpa_supplicant if NM is active.

> **Yocto / RPi image:** wifi-client is **auto-enabled on boot**
> (`SYSTEMD_AUTO_ENABLE=enable` **plus** the shipped `90-iot.preset` —
> the preset is required because the image runs `systemctl preset-all`
> on first boot, which otherwise resets the unit to `preset: disabled`)
> and the `wifi.networks` schema default seeds a placeholder PSK network
> (`ssid="changeme"`), so a fresh image starts the daemon automatically
> and only needs the `wifi.networks` set below to associate. The
> `systemctl enable` step is unnecessary on the image; it remains
> required on bare-metal hosts where the unit ships disabled.
>
> **As of the zero-touch image, `lwm2m-client`, `openvpn-client` and
> `net-router` are also auto-enabled** (`SYSTEMD_AUTO_ENABLE=enable`),
> so the full DM/VPN chain comes up on first boot with no SSH.
> `net-router` runs immediately (no `net.lwm2m.target.ip` needed) and
> publishes the WAN iface; `lwm2m-client` and `openvpn-client` unpark
> once it is `running`. The operator's only step is **commissioning the
> LwM2M bootstrap** (device-UI **LwM2M → Security**: set the Bootstrap
> Server URI + **Generate BS PSK**, then paste the serial + key into the
> cloud-UI Endpoints page). After that the VPN credentials (CA + client
> cert/key) are **delivered automatically over LwM2M bootstrap**
> (Object 2048) and persisted to `/etc/iot/vpn/` — the image creates
> that dir (`2750 engineer:iot`, via tmpfiles) so the `engineer`-run
> lwm2m client can write it and the `openvpn-client` DynamicUser
> (SupplementaryGroups=iot) can read it; no manual cert placement. Until
> the bootstrap PSK is commissioned, `lwm2m-client` parks in its
> provisioning wait-loop (shows `services.lwm2m.client.state="stopped"`
> with no CPU/FD stats — it is alive, just pre-ServiceGate) and
> `openvpn-client` stays down. The `systemctl enable` commands below
> remain required only on bare-metal hosts where the units ship disabled.
>
> **Troubleshooting (older images / regressions).** If the stack is dead
> on boot, check these three invariants (all fixed in PR #212):
> - `systemctl is-enabled iot-ds.service` → if `disabled` with
>   `preset: disabled`, the `90-iot.preset` is missing/overridden.
> - `ls -ld /run/iot` → must be `drwxrwsr-x root iot` (`2775`). If it
>   keeps disappearing, a unit still declares `RuntimeDirectory=iot`
>   (it gets wiped on that unit's stop, taking the ds socket with it).
> - ds socket errors `connect(...): No such file or directory` after
>   ds-server is up usually mean a client lacks `SupplementaryGroups=iot`
>   and crash-loops; `Permission denied` means the same group is missing.

#### Bake WiFi credentials into the image at build time (optional)

Instead of running `ds-cli set wifi.networks …` on every device, you can
seed the credentials at **build time** so a freshly-flashed image associates
to your AP with no runtime step. Drop a `wifi_credentials.lua` into the
recipe's `files/` directory **before building**:

```
yocto/meta-iot/recipes-iot/lwm2m/files/wifi_credentials.lua
```

Copy the committed template and edit it:

```sh
cd <repo-root>
cp yocto/meta-iot/recipes-iot/lwm2m/files/wifi_credentials.lua.sample \
   yocto/meta-iot/recipes-iot/lwm2m/files/wifi_credentials.lua
# edit, e.g.:  return { ssid = "cordoba_2G", password = "whatever" }
```

The build (`iot_git.bb` → `gen_wifi_default.py`) converts it to the
`wifi.networks` JSON below and bakes it into the shipped
`/etc/iot/ds-schemas/wifi.lua` default. Accepted shapes (PSK / open /
WPA-EAP) are documented in `wifi_credentials.lua.sample`. The simple form
maps `password` → `psk`.

- **Optional & safe:** the file is **gitignored** — real passwords never land
  in source control (only the `.sample` is tracked). Absent file ⇒ build is a
  no-op and the `changeme` placeholder default stands.
- **Rebuild on change:** it is wired into `SRC_URI` only when present, so
  editing it triggers a recipe rebuild (no stale sstate).
- **Caveats:** the credential is still **plaintext on the device image**
  (this keeps it out of *git*, not off the *device*), and one build = the
  same credential for every device flashed from that image (fine for a shared
  default AP; per-device provisioning is separate).
- **Verify on device:** `ds-cli get wifi.networks` should show your seeded
  value. Operators can still override it at runtime with `ds-cli set`.

See `apps/docs/tdd-wifi-credentials-seed.md` for the full design.

#### Zero-touch: open the device UI without knowing the IP (mDNS)

With the credential seed above baked in, a flashed image needs **no operator
interaction**: it boots, joins the AP, gets a DHCP IP, and advertises its web
UI over mDNS. On a Mac/Linux/Windows machine on the same LAN, open:

```
http://iot-<serial>.local:8080
```

`<serial>` is the last 8 hex chars of the RPi serial (so two devices on one LAN
don't collide). If you don't know it, browse the advertised service — it shows
up as **"IoT Device UI on iot-<serial>"**:

```sh
avahi-browse -rt _http._tcp        # Linux
dns-sd -B _http._tcp               # macOS
```

How it works on the image:

- **`iot-httpd` auto-starts** (`SYSTEMD_AUTO_ENABLE=enable`), serving the
  Angular device UI on `0.0.0.0:8080` (`http.listen.port`).
- **`iot-hostname.service`** runs once at boot (`iot-set-hostname`), deriving
  `iot-<serial>` from the device-tree serial and applying it before Avahi
  starts.
- **Avahi** (`avahi-daemon`, pulled in by `iot-httpd`) advertises
  `_http._tcp` on port 8080 via `/etc/avahi/services/iot-http.service`, so the
  device resolves as `iot-<serial>.local`.

> ⚠️ **Security:** auto-starting httpd exposes the UI **and** the `/api/v1/*`
> REST API on `0.0.0.0:8080` to everyone on the LAN. That's the point for a
> zero-touch appliance on a trusted home/lab network — but for an untrusted
> network, restrict `http.listen.ip` (bind to localhost / the VPN iface) or
> front it with auth.

Notes / limits:
- The advert hardcodes port **8080**; if you change `http.listen.port` the
  mDNS record is stale (edit `/etc/avahi/services/iot-http.service` to match).
- mDNS sidesteps needing the device IP at all, but the IP (and the rest of the
  lease) is now also recorded in ds and shown in the UI — see below.

See `apps/docs/tdd-wifi-zero-touch-mdns.md` for the full design.

#### DHCP lease details in ds + the device UI

The full DHCP lease is mirrored into the data store and rendered on the device
UI dashboard (IP / mask / gateway / DNS / lease time / domain). A custom
**udhcpc lease hook** (`/usr/share/iot/udhcpc-ds.script`) — which the
wifi-client points udhcpc at with `-s` — does the normal interface config and
then `ds-cli set`s the lease keys:

```sh
ds-cli --socket=/run/iot/data_store.sock get \
    wifi.dhcp.state wifi.dhcp.ip wifi.dhcp.mask wifi.dhcp.gateway \
    wifi.dhcp.dns wifi.dhcp.lease.sec wifi.dhcp.domain
```

`wifi.dhcp.state` is owned by the daemon (`requesting`/`exited`); the hook adds
`bound` plus the data keys, and clears them on lease loss. The REST status
endpoint exposes them as `wifi.dhcp_ip/_mask/_gateway/_dns/_lease_sec/_domain`.
See `apps/docs/tdd-wifi-dhcp-lease.md`.

`wifi.networks` is a JSON array; each entry is one of:

```jsonc
// WPA-PSK (home/personal) — key_mgmt defaults to WPA-PSK when omitted
{ "ssid": "HomeAP", "psk": "correcthorse", "priority": 10 }

// Open network
{ "ssid": "GuestAP", "key_mgmt": "NONE" }

// WPA-Enterprise (identity + password). eap defaults "PEAP",
// phase2 defaults "auth=MSCHAPV2"; ca_cert is optional.
{ "ssid": "CorpAP", "key_mgmt": "WPA-EAP", "eap": "PEAP",
  "identity": "user@corp", "password": "secret",
  "phase2": "auth=MSCHAPV2", "priority": 20 }
```

```sh
sudo systemctl mask NetworkManager   # avoid the conflict gate

ds-cli --socket=/run/iot/data_store.sock set wifi.networks \
    '[{"ssid":"HomeAP","psk":"correcthorse","priority":10}]'

# WPA-Enterprise example:
# ds-cli ... set wifi.networks \
#   '[{"ssid":"CorpAP","key_mgmt":"WPA-EAP","identity":"user@corp","password":"secret"}]'

# Optional: pick a different iface or DHCP client.
ds-cli --socket=/run/iot/data_store.sock set wifi.iface       '"wlan0"'
ds-cli --socket=/run/iot/data_store.sock set wifi.dhcp.client '"udhcpc"'

sudo systemctl enable --now iot-wifi-client.service   # bare-metal only; pre-enabled on the image
journalctl -u iot-wifi-client.service -f | grep "ctrl attached\|assoc\|connected\|dhcp"

# Observe the published wifi.* keys.
ds-cli --socket=/run/iot/data_store.sock get \
    wifi.assoc.state wifi.assoc.ssid wifi.assoc.bssid \
    wifi.dhcp.state wifi.dhcp.ip wifi.signal.rssi
```

The unit ships with `AmbientCapabilities=CAP_NET_ADMIN CAP_NET_RAW`
+ `DeviceAllow=/dev/rfkill rw`; scan needs raw sockets, key mgmt
needs NET_ADMIN, soft-block check needs /dev/rfkill. Plaintext PSK
posture: `wifi.networks[].psk` lives in `/var/lib/iot/data_store.lua`
in plaintext, same as `vpn.cert.path`. Secrets-vault is FUP-L15-1.

The chain closes here: wifi-client brings `wlan0` up → kernel
flags it OPER UP → net-router writes `net.iface.active="wlan0"`
→ openvpn-client's WAN gate fires → tunnel comes up.

### 4. Same ds-cli tuning as the container path

```sh
ds-cli --socket=/run/iot/data_store.sock set iot.endpoint '"urn:dev:my-device-1"'
ds-cli --socket=/run/iot/data_store.sock set iot.lifetime 600
journalctl -u iot-lwm2m-client.service -f | grep "applied iot"
```

---

## services.* control plane

Each iot daemon (net-router, openvpn-client, lwm2m-client,
lwm2m-server, wifi-client) honours `services.<name>.enable`. The
**daemon stays alive** (its systemd unit is still `active
(running)`); the gate flips its **worker** on or off — the
openvpn(8) / wpa_supplicant / udhcpc / nft subprocess. Use
`systemctl stop iot-<name>` if you need to stop the daemon
itself.

ds-server is exempt — it's the substrate, can't self-disable —
so it publishes `services.ds.state` but rejects any set on
`services.ds.enable`. `ds-cli svc list` reflects this with
`ENABLE = n/a` on the ds row.

### Inspect every daemon's gate

```sh
ds-cli --socket=/run/iot/data_store.sock svc list
# NAME               ENABLE   STATE      UPTIME
# ds                 n/a      running    1h12m
# lwm2m.client       true     running
# lwm2m.server       true     running
# net.router         true     running
# openvpn.client     true     running
# wifi.client        true     running
```

### Toggle one daemon's worker

```sh
ds-cli --socket=/run/iot/data_store.sock svc disable openvpn.client
# ok
# wifi.assoc.state, vpn.state etc. stay observable while disabled;
# only the openvpn(8) subprocess gets reaped.

ds-cli --socket=/run/iot/data_store.sock svc enable openvpn.client
# ok
```

### Drill into one row

```sh
ds-cli --socket=/run/iot/data_store.sock svc status openvpn.client
# services.openvpn.client.enable = true
# services.openvpn.client.state  = running
```

### Composition rules (worth knowing)

- `enable=false` **dominates** every domain gate.
  `openvpn-client`: disabled trumps `wan_down` →
  `vpn.gate.reason="disabled"`, not `"wan_down"`.
  `wifi-client`: disabled is checked BEFORE the NM-conflict
  probe → no spurious `wifi.assoc.state="conflict"` writes when
  operator never intended the daemon to run.
- nft state previously installed by `net-router` stays in the
  kernel across a disable cycle; re-enable refreshes. Manual
  flush is the workaround for "fully clean teardown."

### When to use systemctl instead

- The whole daemon is misbehaving and you want the process gone
  (memory leak, deadlock). `systemctl stop iot-<name>` reaps the
  daemon AND its workers.
- Upgrading the daemon binary in place: `systemctl stop … &&
  apt-get install … && systemctl start …`.
- Switching deployment shape (e.g., handing iface management to
  NetworkManager): mask the iot daemon's unit.

`ds-cli svc disable` is for the everyday "I want this OFF for now
but keep the daemon ready to come back fast" path.

### Ephemeral disable (L17b)

`ds-cli svc disable --until-boot <name>` writes the value to an
in-memory overlay only — it is NOT persisted to `data_store.lua`.
The daemon's worker parks as usual, but on the next ds-server
restart the value reverts to the schema default (`true`).
Useful for maintenance windows where you want the service
temporarily off without leaving persistent state:

```sh
# Temporary: survives until next ds-server restart (or a persistent set).
ds-cli svc disable --until-boot openvpn.client

# Permanent: written to data_store.lua; survives reboot.
ds-cli svc disable openvpn.client

# Revert a volatile disable to persistent default.
ds-cli svc enable openvpn.client
```

### Per-key ACL (L17c)

Each schema key can declare a `write_acl` array restricting which
peers may set the key. The ACL is checked via Unix peer credentials
(`SO_PEERCRED` / `getpeereid`) at connection time — no explicit
auth tokens needed. An empty or absent ACL means unrestricted.

Entry format:
- `"uid:N"` — allow uid N (e.g. `"uid:0"` for root)
- `"gid:name"` — allow members of group `name` (e.g. `"gid:iot-operators"`)

Default policy (v1): `services.*.enable` keys are root-only.
State keys are daemon-writable (unrestricted).

```sh
# As root: works.
sudo ds-cli svc disable openvpn.client
# → ok

# As non-root: rejected by ACL.
ds-cli svc disable openvpn.client
# → [ds-cli] svc disable: write_acl(services.openvpn.client.enable):
#   access denied for uid=1000 gid=1000
```

To add a non-root operator group:

```lua
-- In /etc/iot/ds-schemas/services.lua:
["services.openvpn.client.enable"] = {
    type = "boolean", default = true,
    write_acl = {"uid:0", "gid:iot-operators"}
}
```

Then add operators to the group: `sudo usermod -aG iot-operators alice`.

### Dependency graph (L17a)

Each service's `enable` key in `services.lua` declares a
`depends_on` array. When a dependency goes disabled, every
dependent daemon sets `gate.reason="dep_down:<name>"` and parks.
This replaces the old implicit "wan_down" cascade.

The graph in v1:

```
net.router (leaf — no dependencies)
  ↑
  ├── openvpn.client
  ├── wifi.client
  ├── lwm2m.client
  └── lwm2m.server
```

Check the graph at runtime:

```sh
ds-cli svc list
# NAME             ENABLE  STATE      DEPENDS     UPTIME
# net.router       true    running    -           1h12m
# openvpn.client   true    running    net.router  47m
```

If openvpn-client shows `gate.reason="dep_down:net.router"`, the
root cause is that net-router is disabled — check its state first:

```sh
ds-cli svc status openvpn.client
# vpn.gate.reason = dep_down:net.router

ds-cli svc status net.router
# services.net.router.state = disabled

# Fix:
ds-cli svc enable net.router
```

The composition priority across gates is: **dep_down > disabled >
wan_down (or NM-conflict)**. A dependency failure blocks everything
below it regardless of the service's own enable flag or WAN state.

### Rate-limit (L17d)

ds-server can reject rapid re-sets on the same key with a
configurable window (default 0 = disabled). Prevents operator
churn from flooding daemons with spawn/reap cycles.

```sh
# Start ds-server with a 1-second rate-limit window
ds-server rate-limit-ms=1000

# Rapid disable/enable within 1s → second set gets RateLimited
ds-cli svc disable openvpn.client    # ok
ds-cli svc enable openvpn.client     # rejected: rate-limited
```

### Chaos harness (L17d)

`log/L17d/chaos.sh` randomly flips every `services.*.enable` gate
and asserts daemons stay alive through the churn. Run manually:

```sh
bash log/L17d/chaos.sh --cycles=100
```

### HTTP REST API (L18)

`iot-httpd` exposes the data store over HTTP/1.1 on port 8080
(configurable via `http.listen.{ip,port,scheme}` schema keys).
Three endpoints:

```sh
# Read keys
curl -X POST http://localhost:8080/api/v1/db/get \
  -H 'Content-Type: application/json' \
  -d '{"keys":["iot.endpoint","services.net.router.state"]}'

# Write keys
curl -X POST http://localhost:8080/api/v1/db/set \
  -H 'Content-Type: application/json' \
  -d '{"pairs":[{"key":"iot.endpoint","value":"new-value"}]}'

# Long-poll: block until key changes (up to N seconds)
curl 'http://localhost:8080/api/v1/db/get?key=services.net.router.state&timeout=30'
```

Start the server (plain HTTP):

```sh
iot-httpd ds-socket=/var/run/iot/data_store.sock http-port=8080
```

#### Native TLS (https)

iot-httpd terminates TLS itself — no reverse proxy required (though you
can still front it with one and leave the scheme `http`). Point it at a
PEM certificate chain + private key, via CLI or the `http.tls.*` schema
keys:

```sh
# CLI
iot-httpd http-scheme=https http-port=8443 \
    http-cert=/etc/iot/tls/server.crt \
    http-key=/etc/iot/tls/server.key

# …or via ds-cli (read at startup; CLI args win over these)
ds-cli set http.listen.scheme '"https"'
ds-cli set http.tls.cert      '"/etc/iot/tls/server.crt"'
ds-cli set http.tls.key       '"/etc/iot/tls/server.key"'

curl --cacert /etc/iot/tls/ca.crt https://<host>:8443/api/v1/db/get \
    -d '{"keys":["iot.endpoint"]}'
```

TLS floor is 1.2. Set `http.tls.ca` (or `http-ca=`) to a CA bundle to
enforce **mutual-TLS** — clients must then present a certificate that
verifies against it:

```sh
ds-cli set http.tls.ca '"/etc/iot/tls/clients-ca.crt"'
curl --cacert ca.crt --cert client.crt --key client.key \
    https://<host>:8443/api/v1/db/get -d '{"keys":["iot.endpoint"]}'
```

**Reusing the openvpn-client CA.** `http.tls.ca` takes any PEM CA, so you
can point it at the same CA the VPN already trusts (`vpn.ca.path`,
typically `/etc/iot/vpn/ca.crt`) — then any device holding a CA-signed
client cert authenticates to both planes:

```sh
ds-cli set http.tls.ca '"/etc/iot/vpn/ca.crt"'   # = vpn.ca.path
```

Only the CA *certificate* (public) is shared — never the private key. The
trade-off is a single trust domain: every VPN client cert is also valid
for the HTTP API. Use a separate CA if those identity sets should differ.

The key file should be mode `0600`. Under systemd the dynamic `iot-httpd`
user needs read access — place certs where it can reach them (e.g.
`/etc/iot/tls/`, or mount `/etc/iot/vpn:ro` as the openvpn unit does) or
add a drop-in with `LoadCredential=`.

**Hot-reload (no restart).** `http.listen.{ip,port,scheme}` and
`http.tls.{cert,key,ca}` are applied live — `iot-httpd` re-reads them every
~2 s. Rotate a cert by replacing the files and bumping the key:

```sh
ds-cli set http.tls.cert '"/etc/iot/tls/server.crt.new"'   # → re-points + reloads
```

New connections use the new cert; in-flight ones finish on the old one. A
bad cert / in-use port is logged and the current config is kept.
`http.workers` is the exception — changing it needs a restart.

---

## Common operator tasks

### Inspect the schema

```sh
cat /etc/iot/ds-schemas/iot.lua
# Defines: iot.endpoint, iot.lifetime, iot.server.uri,
#          iot.binding, iot.identity, iot.observable
```

### See what's in the store right now

```sh
ds-cli --socket=/run/iot/data_store.sock get \
    iot.endpoint iot.lifetime iot.server.uri \
    iot.binding iot.identity iot.observable
```

### Watch for live changes (e.g. from another operator)

```sh
ds-cli --socket=/run/iot/data_store.sock watch --count=10 \
    iot.endpoint iot.lifetime iot.server.uri
```

### Drop the persistent store (start clean)

```sh
sudo systemctl stop iot-ds
sudo rm /var/lib/iot/data_store.lua
sudo systemctl start iot-ds
```

### Run with a non-default socket path

Both `ds-server` and `ds-cli` accept `--socket=PATH`. The lwm2m binary
takes `ds-sock=PATH` in its `LWM2M_ARGS`. Edit
`/etc/iot/lwm2m-client.env` (bare metal) or pass via env
(`podman run --env LWM2M_ARGS=...`).

---

## Troubleshooting

### "data-store not available at /run/iot/data_store.sock"

ds-server isn't running, or the socket path differs from what the
lwm2m client expects.

- Container: `podman logs iot-ds` — look for "listening on" log line.
- Bare metal: `systemctl status iot-ds.service` — look for the same.
- Both paths: confirm both processes see the same `/run/iot/`
  (named volume / `RuntimeDirectory=`).

### "schema(iot.X): expected Y, got Z"

The ds-cli value didn't match the schema type. See
`/etc/iot/ds-schemas/iot.lua` for the spec. Quote string values:
`'"urn:dev:..."'` (the outer single + inner double quotes survive
shell parsing).

### lwm2m client logs the change but doesn't act

`iot.endpoint` and `iot.server.uri` only trigger their FSM cycle when
the client is in `Registered` state. If the bootstrap / register is
still in flight when the change arrives, the flag stays set; the next
tick after the state stabilises consumes it. Wait one full Update
interval (default 30 s margin under 86400 s lifetime).

CoAPs server-URI rebind is best-effort: the peer swap happens but the
DTLS session isn't re-handshaked. Tracked as FUP-DS-12 if a CoAPs
deployment needs it.

---

## What's NOT in this deploy story (yet)

- **Multi-arch container images.** Builds for whatever arch the host
  has. Cross-builds are a follow-up.
- **TLS / mTLS to ds-server.** The unix socket DAC perms are the
  authentication boundary.
- **`.deb` / `.rpm` packages.** The cmake `install` rule produces a
  packageable tree under `DESTDIR=`; wrapping that into a distro
  package is downstream-specific.
- **systemd-managed container orchestration.** The OCI image bypasses
  systemd; for bare-metal systemd-managed deploys, use the unit files.
