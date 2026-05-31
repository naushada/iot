# packaging/ — L11 deploy artefacts

Multi-target runtime packaging for `ds-server` + `lwm2m`. Two paths:

| Target          | Authoring artefact                                | Install vehicle              |
|-----------------|---------------------------------------------------|------------------------------|
| Bare metal / VM | `systemd/iot-*.service`, `etc-iot/*.env`, `systemd/iot.conf` | cmake install rules (L11/D4) |
| Container       | `Containerfile`, `iot-entrypoint.sh`              | OCI image (this dir, L11/D3) |

The wire spec (data-store EMP) and the application API (libdatastore_client)
are documented separately under [`modules/data-store/docs/`](../modules/data-store/docs/).

---

## OCI image (D3)

### Build

```sh
podman build -f packaging/Containerfile -t iot:l11 .
```

The build is a **two-stage**:

- **Stage 1** re-uses the dev image `naushada/iot:latest` (ACE_TAO 7.0.0,
  mongocxx, OpenSSL 3.1.1, tinydtls). Compiles + `make install
  DESTDIR=/staging`. Renames `*.lua.example` back to `*.lua` for in-image
  defaults (no copy-on-edit safety needed — image *is* the upgrade unit).
  Strips the binaries to save ~20 MB.
- **Stage 2** starts from `ubuntu:22.04` with only `libreadline8 libssl3
  zlib1g ca-certificates --no-install-recommends`. Copies in:
  - `libACE.so.7.0.0` from `/usr/local/ACE_TAO-7.0.0/lib/`
  - `lib{bson,bsoncxx,mongoc,mongocxx}*` from `/usr/local/lib/`
  - The three binaries + `/etc/iot/`
- `ldconfig` indexes both lib trees via two files in `/etc/ld.so.conf.d/`.

### Run

The entrypoint dispatches on `IOT_ROLE`:

```sh
# ds-server
podman run -d --name iot-ds -e IOT_ROLE=ds iot:l11

# lwm2m client (needs ds-server reachable on /run/iot)
podman run -d --name iot-client -e IOT_ROLE=client \
    --volumes-from iot-ds iot:l11

# lwm2m server (Leshan-style bootstrap + DM)
podman run -d --name iot-server -e IOT_ROLE=server \
    --volumes-from iot-ds -p 5683:5683/udp -p 5685:5685/udp iot:l11
```

The entrypoint also accepts a binary name as the first arg — useful
for debugging:

```sh
podman exec iot-ds ds-cli --socket=/run/iot/data_store.sock get iot.endpoint
podman run --rm iot:l11 ds-cli --help
```

### Image size

| Stage                          | Approx size |
|--------------------------------|------------:|
| Build stage                    | ~3.5 GB     |
| Runtime stage (this image)     | **~119 MB** |

Above the soft 100 MB target. Routes to shrink, in rough effort order:

1. **Use `debian:bookworm-slim` base instead of `ubuntu:22.04`** — saves
   ~40 MB but introduces an apt-source switch.
2. **Drop mongocxx/bsoncxx/mongoc/bson runtime** — only the lwm2m binary
   links them, and only because db_adapter.cpp is still in the build.
   Removing the mongo-mirror stubs would save ~15 MB.
3. **`upx --best` on the binaries** — saves ~5–10 MB; trades startup
   speed and complicates core-dump triage.

None of these are blocking for v1. Filed as `FUP-L11-1` (see [L11
plan](../log/L11/plan.md)).

### Verified smokes

Both run inside the same container against the embedded ds-server.

| Smoke                                                         | Result                                               |
|---------------------------------------------------------------|------------------------------------------------------|
| `ds-server` boots, listens on `/run/iot/data_store.sock`      | "listening on /run/iot/data_store.sock (mode 0660)"  |
| `ds-cli set iot.endpoint '"…"'` round-trips                   | `ok` + `get` returns the value                       |
| `ds-cli set iot.lifetime -1` rejected by schema               | `schema(iot.lifetime): -1 below min 0`               |
| `lwm2m role=client` (via `IOT_ROLE=client`) picks up live cfg | `endpoint from data-store: urn:dev:oci-client`       |
|                                                               | `Registration lifetime from data-store: 3600`        |

---

## Bare-metal install (D1 + D2 + D4)

```sh
cmake -B build -DCMAKE_INSTALL_PREFIX=/usr   # use --prefix=/usr/local for /usr/local layout
make -C build -j"$(nproc)"
sudo make -C build install                   # use DESTDIR=… for packaging
sudo systemctl daemon-reload
sudo systemctl enable --now iot-ds.service iot-lwm2m-client.service
```

Layout the install lands (under `--prefix=/usr`):

```
/usr/bin/{ds-server, ds-cli, lwm2m}
/usr/lib/<triplet>/libdatastore_client.a
/usr/include/data_store/{client,proto,value}.hpp
/usr/lib/systemd/system/iot-{ds, lwm2m-client, lwm2m-server}.service
/usr/lib/tmpfiles.d/iot.conf
/etc/iot/ds-schemas/iot.lua
/etc/iot/{lwm2m-client,lwm2m-server}.env
/etc/iot/config/{security,server,device,accessControl,firmware}Object/*.lua.example
```

`.lua.example` files: operators copy to `.lua` + edit. Package upgrades
won't clobber edited copies. The OCI image skips this convention (see
above).

### Disable packaging install (dev workflow)

```sh
cmake -B build -DIOT_PACKAGING_INSTALL=OFF
make -C build install                        # binaries + lib + headers only
```

---

## Live config from `ds-cli`

Once a deployment is running, operators tune via `ds-cli`:

```sh
ds-cli --socket=/run/iot/data_store.sock set iot.endpoint '"urn:dev:prod-1"'
ds-cli --socket=/run/iot/data_store.sock set iot.lifetime 600
ds-cli --socket=/run/iot/data_store.sock set iot.server.uri '"coap://10.0.0.5:5683"'
ds-cli --socket=/run/iot/data_store.sock get iot.endpoint iot.lifetime iot.server.uri
```

Changes hot-apply via `DsConfig::on_change` → `RegistrationClient`
setters (FUP-DS-9, FUP-DS-10, FUP-DS-11). See
[`modules/data-store/docs/protocol.md`](../modules/data-store/docs/protocol.md)
for full schema + rejection semantics.
