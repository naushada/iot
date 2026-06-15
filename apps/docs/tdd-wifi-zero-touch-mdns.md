# TDD Plan — Zero-Touch Device-UI Discovery via mDNS

Status: **IN PROGRESS** — design agreed. Yocto/image side only. Most of the
change is recipe + systemd config (not unit-testable without a build); the one
pure-logic bit (serial → hostname sanitization) is validated with a host shell
check.

### Implementation progress

| Task | State | Notes |
| --- | --- | --- |
| A — auto-start `iot-httpd` | ⬜ TODO | `iot_git.bb`: `SYSTEMD_AUTO_ENABLE:${PN}-httpd` `disable` → `enable`. UI must be up on boot. |
| B — `iot-set-hostname` (serial → hostname) | ⬜ TODO | Oneshot script: derive `iot-<serial-suffix>` and apply via `hostnamectl`. |
| C — `iot-hostname.service` | ⬜ TODO | `Type=oneshot`, `Before=avahi-daemon.service`, auto-enabled. |
| D — Avahi `_http._tcp` advert | ⬜ TODO | `/etc/avahi/services/iot-http.service` advertising port 8080 on `%h`. |
| E — pull in `avahi-daemon` | ⬜ TODO | `RDEPENDS:${PN}-httpd += "avahi-daemon"`. |
| F — recipe packaging | ⬜ TODO | SRC_URI + do_install + SYSTEMD_SERVICE + FILES for the new units/files. |

## 1. Goal

Zero touch: flash SD → boot → wifi-client joins the AP (already auto-started) →
DHCP IP → user opens the device-iot UI in a browser **without knowing the IP**.

The IP is deliberately NOT the discovery mechanism. The device advertises
itself over mDNS, so the user opens:

```
http://iot-<serial>.local:8080
```

`<serial>` is the last 8 chars of the RPi serial — collision-safe when several
devices share a LAN. A service browser (Bonjour / `avahi-browse` / `dns-sd -B
_http._tcp`) lists it as **"IoT Device UI on iot-<serial>"**.

## 2. Current gaps (verified)

- `iot-httpd` serves the UI on **0.0.0.0:8080** (`http.lua` defaults) from
  `/usr/share/iot/www`, but ships **disabled** (`SYSTEMD_AUTO_ENABLE:${PN}-httpd
  = "disable"`, `iot_git.bb:437`).
- No mDNS responder in the image (no avahi/nss-mdns anywhere).
- Hostname defaults to the MACHINE name (`raspberrypi3-64`); not per-device,
  not advertised.

(Out of scope here: populating `wifi.dhcp.ip` and sending the DHCP hostname
option — useful but separate; the chosen approach doesn't need the IP at all.)

## 3. Design

### 3.1 httpd auto-start
Flip `SYSTEMD_AUTO_ENABLE:${PN}-httpd` → `enable`. The unit already orders
`After=iot-ds.service network-online.target`.

⚠️ **Security note:** this exposes the device UI + REST API on `0.0.0.0:8080`
to anyone on the LAN by default (previously operator opt-in). That is the
intent for a zero-touch device, but call it out; lock down via `http.listen.ip`
/ auth if the deployment is not a trusted LAN.

### 3.2 Hostname from serial (`iot-set-hostname`)
Read the serial (device-tree `serial-number`, fallback `/proc/cpuinfo`
`Serial`), lowercase, strip to `[a-z0-9]`, keep the last 8 chars, and set
`hostname = iot-<suffix>` via `hostnamectl set-hostname` (fallback:
`/etc/hostname` + `hostname`). Empty serial → `iot-device`.

Run by `iot-hostname.service` (`Type=oneshot`, `RemainAfterExit=yes`,
`After=local-fs.target`, `Before=avahi-daemon.service`, auto-enabled) so the
name is set before Avahi advertises. (Avahi also tracks hostname changes, so
ordering is belt-and-suspenders.)

### 3.3 Avahi advertisement
Ship `/etc/avahi/services/iot-http.service`:

```xml
<service-group>
  <name replace-wildcards="yes">IoT Device UI on %h</name>
  <service> <type>_http._tcp</type> <port>8080</port> </service>
</service-group>
```

`%h` expands to the system hostname → `iot-<serial>.local`. Port hardcoded to
the httpd default 8080 (note: if an operator changes `http.listen.port`, the
advert is stale).

### 3.4 Image inclusion
`RDEPENDS:${PN}-httpd += "avahi-daemon"` pulls the mDNS responder in wherever
iot-httpd is installed (the full gateway image). `avahi-daemon` is enabled by
its own recipe.

## 4. Recipe wiring (`iot_git.bb`)
- `SRC_URI +=` `iot-set-hostname`, `iot-hostname.service`, `iot-http.avahi.service`.
- `do_install` (systemd branch): install the script to `${bindir}`, the unit to
  `${systemd_system_unitdir}`, the avahi file to `${sysconfdir}/avahi/services/`.
- `SYSTEMD_SERVICE:${PN}-httpd = "iot-httpd.service iot-hostname.service"`.
- `SYSTEMD_AUTO_ENABLE:${PN}-httpd = "enable"`.
- `FILES:${PN}-httpd +=` the avahi service file + the hostname script.
- `RDEPENDS:${PN}-httpd += "avahi-daemon"`.

## 5. Verification
- Host shell check of the serial→hostname sanitization (sample serial →
  `iot-3d1f9c2e`).
- **Deferred (needs hardware/build):** real RPi boot → `avahi-browse -rt
  _http._tcp` from a laptop shows `iot-<serial>`, and `http://iot-<serial>.local:8080`
  loads the UI; `hostnamectl` shows the derived name.

## 6. Out of scope
- `wifi.dhcp.ip` population + DHCP hostname option (option 12) — separate, not
  required for mDNS discovery.
- Port-80 redirect so the `:8080` can be dropped (would need an nft rule).
- mDNS name collisions if two devices truly share the same 8-char suffix
  (astronomically unlikely; full serial is available if ever needed).
