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
