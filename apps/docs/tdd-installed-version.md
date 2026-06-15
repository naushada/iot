# TDD — Surface each device's installed firmware version in the cloud

Status: PLAN. Author: this session (2026-06-15).

## 1. Goal

The cloud **Software Update** page has an **Installed** column that today
shows `—` for every device (see the Update Status table reading `0 jobs`).
We want it to show the firmware version the device is *actually running*,
independent of whether an OTA job has ever been pushed.

### Why it's broken today

`software-update.component.ts` derives the column from the OTA job feed:

```ts
installedVersion(serial: string): string {
  const s = this.status.find(x => x.serial === serial);   // cloud.update.status
  return s ? s.version : '';
}
```

So the value is:
- **Empty until an OTA job runs** — there's no row in `cloud.update.status`
  for a device that's never been pushed → `—`.
- **The *pushed* version, not the running one** — even when populated it
  echoes what the cloud *sent*, not what the device confirms it booted.

And the deeper gap: the cloud never collects the device's running version.
The DM registration payload is only
`{ endpoint, registered, last_seen_unix }`
(`apps/cloud/server/src/main.cpp:109-115`); `lwm2m-dm` never reads the
Device object.

## 2. Approach — read LwM2M Object 3 / Resource 3

The standard LwM2M way: the device's running version is **Object 3
(Device), instance 0, Resource 3 (Firmware Version)** — path **`/3/0/3`**.
Short Server ID does **not** appear in the resource path; it only keys the
*server registry*. So the operation is a plain server-initiated
`GET /3/0/3` against the registered client, reusing the exact machinery the
DM tick already uses for the liveness poll (`Read /3/0/0`) and the OTA push
(`Write /5/0/1` + `Execute /5/0/2`).

### 2a. Pre-req: make `/3/0/3` tell the truth on the device

Today `/3/0/3` returns the compiled-in constant `"0.1"`
(`apps/src/lwm2m_object_3_device.cpp:23`, `firmwareVer{"0.1"}`), applied as
a per-RID fallback over `deviceObject/0.lua`. The real running version is
`iot.version` (httpd writes `IOT_VERSION` there at startup —
`modules/http-server/src/main.cpp:344`).

So **before** the read is meaningful we must bind `/3/0/3` to the live
version. Add an optional reader to `DeviceHooks`:

```cpp
// apps/inc/lwm2m_object_3_device.hpp
struct DeviceHooks {
    ...
    /// Optional reader for RID 3 (Firmware Version). When set, /3/0/3
    /// returns its result live instead of the 0.lua / compiled fallback.
    /// Production wires this to read iot.version from ds.
    std::function<std::string()> firmwareVersion;
};
```

`install_device` uses it for RID 3 when present (live `read_only_string`
backed by the hook), else keeps the existing fallback. In the client
`apps/src/main.cpp` we wire it to read `iot.version` from ds (falling back
to `IOT_VERSION` if the key is empty). `iot.version` only changes across a
restart, so a per-read ds get is cheap and always current.

## 3. The read path (cloud / `lwm2m-dm`)

`lwm2m-dm` already has a 30s server tick that walks `registry->all()` and
sends server-initiated ops via `ctx->send_async(req, peerHost, peerPort)`
(`apps/src/main.cpp:428-626`). Add a `Read /3/0/3` alongside the existing
`Read /3/0/0`:

```cpp
auto req = ::lwm2m::dmsrv::build_read(next_msgid(), vtok,
             /*oid*/3, /*iid*/0, /*rid*/3, /*accept*/-1);
ctx->send_async(req, reg.peerHost, reg.peerPort);
```

### 3a. Capturing the response

Responses to server-initiated reads land in `m_dmRspHandler`
(`coap_adapter.cpp:1058-1061` / `1284-1287`): any response-class inbound
(code class ≥ 2) on the server role is routed there with the raw
`CoAPMessage`. The handler is **unset today** — nothing consumes the
liveness-poll reply. We set it via `CoAPAdapter::dmResponseHandler(...)` on
the server adapter (reachable through the DTLS adapter's
`coapAdapter()`).

**Correlation by token.** The handler gets only the `CoAPMessage` (token +
payload), no path and no reliable peer on the no-DTLS path. So we encode
the endpoint in the token: a 4-byte token = tag `0x06` + a 24-bit sequence,
with a mutex-guarded `seq → endpoint` map populated when the read is issued
and consumed in the handler. (The liveness/OTA/cert tokens stay the
existing 1-byte `0x03`/`0x05`/`0x04`; `0x06` is the new version-read tag.)
The version string is the response `payload` (text/plain `/3/0/3`).

### 3b. Publishing the version

`lwm2m-dm` is the **sole writer** of `cloud.lwm2m.registrations`
(`{endpoint, registered, last_seen_unix}`). We add a `version` field:

```jsonc
[{ "endpoint": "100000abcd", "registered": true,
   "last_seen_unix": 1718123456, "version": "1.2.0" }]
```

The version arrives async (read reply), not at register time, so it must
**survive `publish_regs` rebuilds** from the `ClientRegistry` (which has no
version). Keep a process-local `epVersions` map (endpoint → version),
updated by the response handler; `publish_regs` includes
`epVersions[endpoint]` when building each row, and the handler triggers a
`publish_regs()` on a fresh value so the UI updates promptly.

(We deliberately do **not** add `version` to `ServerRegistration` — that
struct is rebuilt verbatim from registration frames and would clobber it.)

## 4. The merge (`iot-cloudd`)

`reconcile_registrations` (`apps/cloud/server/src/main.cpp:172-`) already
merges `registered` + `last_seen_unix` from `cloud.lwm2m.registrations`
into `cloud.endpoints` (a separate key to avoid a two-writer clobber on
`tun_ip`/`proxy_port`). Extend it to carry `version` →
`cloud.endpoints[].installed_version`.

```jsonc
// cloud.endpoints
{ "endpoint": "...", "state": "online", "tun_ip": "...",
  "proxy_port": 10000, "registered": true,
  "last_seen_unix": 1718123456, "installed_version": "1.2.0" }
```

## 5. The UI (cloud-ui)

`software-update.component.ts`:
- `Ep` interface gains `installed_version?: string`.
- `installedVersion(ep)` reads `e.installed_version` from the endpoint row
  (already polled via `getCloudEndpoints()`), not `cloud.update.status`.
- The Update Status table's own `Version` column is unchanged (it's the
  per-job pushed version, which is still the right thing there).

No new ds key on the UI side; `cloud.endpoints` already flows to the page.

## 6. Files touched

| Area | File | Change |
|------|------|--------|
| Device obj | `apps/inc/lwm2m_object_3_device.hpp` | add `firmwareVersion` hook |
| Device obj | `apps/src/lwm2m_object_3_device.cpp` | RID 3 uses hook when set |
| Device wiring | `apps/src/main.cpp` (client) | wire hook → read `iot.version` |
| DM server | `apps/src/main.cpp` (server tick) | issue `Read /3/0/3`; set `dmResponseHandler`; `epVersions` map; version in `publish_regs` |
| Cloud merge | `apps/cloud/server/src/main.cpp` | `version` → `installed_version` in `reconcile_registrations` |
| UI | `apps/cloud/ui/src/app/software-update/software-update.component.ts` | read `installed_version` from endpoint |

No schema change: `cloud.lwm2m.registrations` / `cloud.endpoints` are opaque
JSON blobs (free-form arrays), so adding a field needs no `.lua` edit.

## 7. Failure modes

- **Device offline / no reply** — no response, `epVersions` keeps its last
  value (or none → UI shows `—`). Acceptable: an offline device's last-known
  version is still useful; a never-seen device legitimately shows `—`.
- **Old device firmware (no `firmwareVersion` hook)** — `/3/0/3` still
  returns the `0.lua`/`"0.1"` fallback; the cloud surfaces whatever the
  device reports. No crash, just a stale-looking value until the device is
  updated to firmware that binds the hook.
- **Token collision** — the `0x06` tag namespaces version reads away from
  the `0x03`/`0x04`/`0x05` ops; the 24-bit sequence wraps at 16M reads
  (≈ centuries at 30s/tick).

## 8. Test plan

1. **Unit (device obj):** `install_device` with a `firmwareVersion` hook
   returning `"9.9.9"` → reading `/3/0/3` yields `"9.9.9"`; without the hook
   → the `0.lua`/compiled fallback (regression guard).
2. **Unit (DM correlation):** feed a synthetic `/3/0/3` response with a
   tagged token into `dmResponseHandler` → `epVersions[endpoint]` set and a
   `cloud.lwm2m.registrations` row carries `version`.
3. **Unit (merge):** `reconcile_registrations` with a `version` in
   `cloud.lwm2m.registrations` → `installed_version` in `cloud.endpoints`.
4. **E2E (podman, per `reference_e2e_test_recipe`):** bring up device↔cloud,
   register, wait one tick, confirm the cloud Endpoints/Software page shows
   the device's `IOT_VERSION`. Push an OTA, confirm the value follows the
   device after it reboots into the new version.

## 9. Decisions

- **D1 — Read `/3/0/3`, not a custom push.** Standard LwM2M Device object;
  reuses the existing server-initiated read machinery. Short Server ID is
  irrelevant to the resource path.
- **D2 — Bind `/3/0/3` to `iot.version` on the device.** Without this the
  read returns the static `"0.1"` and the feature is meaningless.
- **D3 — Carry the version in `cloud.lwm2m.registrations`, merge to
  `cloud.endpoints.installed_version`.** Keeps `lwm2m-dm` the single writer
  of the registration key and reuses the existing two-key merge that avoids
  clobbering `tun_ip`/`proxy_port`.
- **D4 — `epVersions` side-map, not a `ServerRegistration` field.** The
  registration struct is rebuilt from frames and would clobber an async
  value.
