# Deploying iot

Two paths, your choice:

| Path        | When                                                                                    | Time-to-first-`get`  |
|-------------|------------------------------------------------------------------------------------------|----------------------|
| **Container** | Cloud, edge devices that already run a container engine, local hacking                | 5 min                |
| **Bare metal** | Single-purpose appliances, embedded Linux without container runtime                  | 15 min               |

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
seed the only required key and enable:

```sh
ds-cli --socket=/run/iot/data_store.sock set net.lwm2m.target.ip '"192.168.10.5"'
# Optional: rename per-class iface keys to match your host
ds-cli --socket=/run/iot/data_store.sock set net.iface.eth.name      '"eth0"'
ds-cli --socket=/run/iot/data_store.sock set net.iface.wifi.name     '"wlan0"'
ds-cli --socket=/run/iot/data_store.sock set net.iface.cellular.name '"wwan0"'

sudo systemctl enable --now iot-net-router.service
journalctl -u iot-net-router.service -f | grep "ruleset applied\|metric\|active"
```

The unit ships with `AmbientCapabilities=CAP_NET_ADMIN`; that's
enough for `nft -f` and `ip route replace` under DynamicUser. Default
forwarded ports are `80,443,5684` (HTTP/HTTPS + LwM2M/CoAP-over-DTLS)
DNAT'd to `net.lwm2m.target.ip`. Override via `net.forward.ports`
and add operator drops/forwards via the JSON `net.custom.rules`
schema key (see `/etc/iot/ds-schemas/net.lua`).

For the **wifi-client** unit (L15: wpa_supplicant + DHCP-client
supervisor; publishes `wifi.*`), seed the network list and enable.
NetworkManager MUST be masked on hosts running wifi-client — the
daemon's startup probe writes `wifi.assoc.state="conflict"` and
refuses to spawn wpa_supplicant if NM is active.

```sh
sudo systemctl mask NetworkManager   # avoid the conflict gate

ds-cli --socket=/run/iot/data_store.sock set wifi.networks \
    '[{"ssid":"HomeAP","psk":"correcthorse","priority":10}]'

# Optional: pick a different iface or DHCP client.
ds-cli --socket=/run/iot/data_store.sock set wifi.iface       '"wlan0"'
ds-cli --socket=/run/iot/data_store.sock set wifi.dhcp.client '"udhcpc"'

sudo systemctl enable --now iot-wifi-client.service
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
