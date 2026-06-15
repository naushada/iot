# TDD Plan — DHCP Lease → Data Store → device-iot UI

Status: **IN PROGRESS** — design agreed. Surfaces the full DHCP lease (IP,
mask, gateway, DNS, lease time, domain) in ds so the device UI can render it.
Mechanism: a custom **udhcpc hook script** writes the lease via `ds-cli`; the
wifi-client daemon points udhcpc at it with `-s`.

### Implementation progress

| Task | State | Notes |
| --- | --- | --- |
| A — ds schema keys | ⬜ TODO | `wifi.lua`: add `wifi.dhcp.mask/gateway/dns/lease.sec/domain/obtained.unix` (Viewer). |
| B — udhcpc hook script | ⬜ TODO | `udhcpc-ds.script`: delegate networking to the system default.script, then `ds-cli set` the lease fields + `state=bound`; clear data on `deconfig`. |
| C — daemon `-s` wiring | ⬜ TODO | `process.cpp` `spawn_dhcp` gains optional `script` arg (udhcpc `-s`); `supervisor.cpp` passes the hook path gated on `::access`. Unit test. |
| D — REST exposure | ⬜ TODO | `handler.cpp`: add the 6 keys to the status `get()` + JSON mappings. |
| E — device UI | ⬜ TODO | `WifiStatus` interface + dashboard rows (IP / mask / gateway / DNS / lease / domain). |
| F — recipe + docs | ⬜ TODO | Ship the hook in the wifi-client package; DEPLOY.md. |

## 1. Background / why a hook

udhcpc delivers the full lease ONLY to its `-s` hook script, via environment
variables — not on stdout, and lease-time/DNS aren't readable back from the
interface. So the lease writer must be the hook. Today wifi-client runs
`udhcpc -i wlan0 -f -q` with **no `-s`**, using busybox's built-in
default.script (configures the interface, never touches ds). Nothing writes the
lease to ds; even `wifi.dhcp.ip` is unpopulated.

## 2. Key ownership (avoid daemon/hook conflict)

The daemon (`supervisor.cpp`) sets `wifi.dhcp.state` to `"requesting"` (on
spawn) and `"exited"` (on disconnect), and `wifi.pid.dhcp`. It NEVER sets
`"bound"` and never writes the IP. Division of labour:

- **Daemon keeps**: `wifi.dhcp.state` ∈ {requesting, exited}, `wifi.pid.dhcp`.
- **Hook owns**: the lease DATA keys + `wifi.dhcp.state="bound"` (the one state
  only the hook can know). On `deconfig` the hook clears the data keys but does
  **not** write `state` (avoids racing the daemon's `"requesting"` — udhcpc
  fires `deconfig` at startup right around the daemon's spawn).

`ds-cli` writes are not blocked by the schema `access="Viewer"` field (that's
HTTP-layer metadata; the socket enforces `write_acl`/`read_acl`, and
`wifi.dhcp.*` declare none — a root-run udhcpc hook can write them).

## 3. New ds keys (`wifi.lua`, Viewer/write keys)

| key | type | meaning |
| --- | --- | --- |
| `wifi.dhcp.mask` | string | subnet mask (`$subnet`) |
| `wifi.dhcp.gateway` | string | default gateway(s) (`$router`) |
| `wifi.dhcp.dns` | string | nameserver(s) (`$dns`, space-separated) |
| `wifi.dhcp.lease.sec` | integer | lease time in seconds (`$lease`) |
| `wifi.dhcp.domain` | string | DNS domain (`$domain`) |
| `wifi.dhcp.obtained.unix` | integer | unix time the lease was bound (for UI countdown) |

(`wifi.dhcp.ip`, `wifi.dhcp.state` already exist.) These are written by the
hook and read by the REST status endpoint; the daemon does not read them, so
they don't join `kReadKeys`/`kWriteKeys`.

## 4. Hook script (`udhcpc-ds.script`)

```sh
case "$1" in
  bound|renew)
    <delegate to /usr/share/udhcpc/default.script for the actual ifconfig>
    ds-cli set wifi.dhcp.ip "$ip"; mask/gateway/dns/domain/lease.sec/obtained.unix
    ds-cli set wifi.dhcp.state '"bound"'
    ;;
  deconfig)
    <delegate>
    clear wifi.dhcp.{ip,mask,gateway,dns,domain}; lease.sec=0   # NOT state
    ;;
  leasefail|nak) <delegate>; ;;     # daemon owns state here
esac
```

Socket: `--socket=${IOT_DS_SOCK:-/run/iot/data_store.sock}`. Delegating to the
distro default.script keeps connectivity intact (we only override `-s`).

## 5. Daemon wiring

- `process.cpp` `spawn_dhcp(dhcp_path, iface, script = "")`: for udhcpc, append
  `-s <script>` when `script` non-empty. dhclient unchanged (out of scope).
- `supervisor.cpp`: define `kDhcpHookScript = "/usr/share/iot/udhcpc-ds.script"`;
  pass it to `spawn_dhcp` only when `::access(path, F_OK) == 0`, so dev/test
  hosts without the script keep today's behaviour.

## 6. REST + UI

- `handler.cpp`: add the 6 keys to the status `get()` list and map them
  (`wifi.dhcp.mask → wifi.dhcp_mask`, `gateway → dhcp_gateway`, `dns →
  dhcp_dns`, `lease.sec → dhcp_lease_sec` (int), `domain → dhcp_domain`,
  `obtained.unix → dhcp_obtained_unix` (int)).
- `iot-ui` `app-globals.ts` `WifiStatus`: add the optional fields.
- `dashboard.component.html`: add conditional rows under the WiFi card.

## 7. Test strategy

1. **process_test**: `spawn_dhcp` with a script arg appends `-s <script>` for a
   udhcpc-basename binary; omits it when empty; dhclient never gets `-s`.
   (Assert via a thin seam — spawn a `/bin/sh` stand-in named `udhcpc` and
   confirm behaviour, or factor the argv builder into a testable free function.)
2. **schema_test**: the new keys are declared with the right types.
3. Hook script: `sh -n` syntax + a host dry-run with faked env + a stub `ds-cli`
   on PATH asserting the emitted `set` calls.
4. C++ suite green in podman; iot-ui production build green in podman.

## 8. Out of scope / caveats

- dhclient hook (different mechanism); only udhcpc covered.
- The daemon still owns non-bound `wifi.dhcp.state`; brief eventual-consistency
  windows are acceptable for a status readout.
- On-device end-to-end (real lease → UI shows it) deferred to the RPi/build env.
