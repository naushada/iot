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
```

For the server role (Bootstrap + DM), substitute `IOT_ROLE=server` and
expose UDP ports:

```sh
podman run -d --name iot-server -e IOT_ROLE=server \
    -v iot-run:/run/iot \
    -p 5683:5683/udp -p 5685:5685/udp \
    iot:l11
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

### 4. Same ds-cli tuning as the container path

```sh
ds-cli --socket=/run/iot/data_store.sock set iot.endpoint '"urn:dev:my-device-1"'
ds-cli --socket=/run/iot/data_store.sock set iot.lifetime 600
journalctl -u iot-lwm2m-client.service -f | grep "applied iot"
```

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
