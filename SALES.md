# iot — LwM2M Device Management & Secure-Tunnel Gateway Platform

**An open-source (MIT) IoT device-management platform**: a single C++17 stack
that is both the **cloud LwM2M authority** (Bootstrap + Device Management) and
the **on-device agent**, joined by a **per-device OpenVPN tunnel** that lets an
operator reach each NAT'd device's local web UI from one cloud dashboard.

> One codebase. One `lwm2m` binary that plays server *or* client. From a single
> Raspberry Pi to a fleet behind NAT — provisioned, monitored, tunnelled, and
> updated over the air.

---

## 1. What you get

| Capability | Summary |
|------------|---------|
| **OMA LwM2M 1.1.1** | Bootstrap, Registration, Device Management, Read/Write/Create/Delete/Execute, Observe/Notify, Discover — over CoAP/UDP with DTLS (PSK). |
| **Zero-touch-ish provisioning** | Per-device PSK provisioning keyed by serial; identity derived from the serial, never hardcoded. Credentials live in the data store and are resolved live at the handshake. |
| **Per-device secure tunnel** | The cloud runs an OpenVPN server, mints a per-device X.509 client cert, and pushes it to the device over LwM2M Object 2048. Each device gets a stable tunnel IP. |
| **Device-UI-over-VPN** | Every device serves its own web UI behind NAT; the cloud reverse-proxies each one (per-device nftables DNAT) so operators reach it with one click — **Launch UI**. |
| **Cloud Operator Dashboard** | Angular 14 + Clarity SPA: endpoints, VPN, routing/forwarding, LwM2M config, services, logs, multi-user login (SHA-256, Admin/Viewer roles). |
| **On-device UI** | The same UI on the device for local commissioning, WiFi/WAN, VPN, and live LwM2M/VPN status. |
| **OTA software update** | Cloud firmware feed + LwM2M Object 5; multi-select push from the cloud UI upgrades a whole list of devices in one shot — a single `.ipk` or a `.tar.gz` bundle of the entire `iot-*` userspace. The device pulls **direct over the public WAN (not the tunnel)** with retry + resume, verifies sha256 (pinned over the trusted DTLS channel), installs, and restarts — detached so it survives replacing the running binary, and so a stuck VPN can't block its own fix. |
| **Schema-enforced data store** | A small AF_UNIX key/value store (`ds-server`) is the single IPC backbone; typed, schema-validated, per-key ACLs, live watch + hot-reload, write-through persistence. |
| **Service control plane** | Per-daemon enable/disable/restart, dependency gating, rate-limiting, live log level + log viewer. |
| **Build & ship anywhere** | Dev + thin runtime OCI images, docker-compose stacks for cloud and device, and a full **Yocto** layer that builds a bootable Raspberry Pi 3B image **and** the per-daemon `.ipk` feed — entirely inside a container, no host Yocto install. |

---

## 2. Who it's for

- **Gateway / CPE vendors** who need a standards-based (LwM2M) management plane
  plus remote access to a device's local UI without punching holes in customer
  firewalls.
- **System integrators** building a managed-device offering on Raspberry-Pi-class
  hardware who want the whole stack — agent, cloud, tunnel, OTA, dashboard — in
  one MIT-licensed tree they can fork and brand.
- **Teams evaluating LwM2M** who want a readable C++ reference implementation
  that interops with standard servers (verified against Eclipse Leshan).

---

## 3. Architecture (at a glance)

```
        Operator browser
              │  HTTPS
        ┌─────▼─────────── Cloud VM ───────────────┐
        │ iot-cloudd  ── lwm2m-bs (CoAPs :5684)      │
        │   │   │     ── lwm2m-dm (CoAPs :5683)      │      per-device
        │   │   └── OpenVPN server (:1194) ──────────┼──── DTLS + VPN ───►  Device (RPi)
        │   └── iot-httpd (REST + Cloud UI)          │                      ├ lwm2m client
        │       per-device DNAT → device UI over tun │                      ├ openvpn client
        └── ds-server (schema'd KV, all IPC) ────────┘                      ├ iot-httpd (device UI)
                                                                            └ ds-server
```

All daemons talk only through the data store — no hidden HTTP between services.
The same `lwm2m` binary runs in `server` role on the cloud and `client` role on
the device.

---

## 4. Deployment options

| Path | Use |
|------|-----|
| **docker-compose (cloud)** | 5-service cloud stack on any Linux VM. |
| **docker-compose (device)** | Device stack for dev / CI / non-RPi e2e. |
| **Yocto image (RPi3B)** | Flashable SD-card image; systemd-managed daemons; `.ipk` OTA feed. |
| **Bare-metal / systemd** | GNUInstallDirs install + systemd units (see `DEPLOY.md`). |

---

## 5. Security posture

- **LwM2M transport:** DTLS with per-endpoint PSK; identities derived from the
  serial; secrets stored write-only with per-key ACLs and resolved live at the
  handshake (no shared/baked keys in the shipped stack).
- **VPN:** per-device X.509 client certificates signed by a runtime CA that the
  cloud generates once and persists; cert delivery rides LwM2M Object 2048;
  cert rotation hot-reloads the tunnel.
- **Cloud/device UI:** session-cookie auth (SHA-256), Admin/Viewer roles, idle +
  keep-alive timeouts; HTTPS supported when you supply a cert.
- **Data store:** Unix peer-credential ACLs per key; sensitive credential keys
  are write-only / group-restricted.

> Note: this is solid engineering security, **not** a formally certified or
> independently audited security product — see §7.

---

## 6. Licensing

- **This project's source code is MIT** (see [`LICENSE`](LICENSE)) — permissive,
  commercial use / modification / redistribution allowed, no copyleft on *our*
  code, **no warranty** (provided "as is").
- **Third-party components are under their own licenses.** Distributed
  binaries/images bundle or invoke components such as ACE/TAO (DOC/BSD-style),
  OpenSSL 3 (Apache-2.0), Lua (MIT), nlohmann/json (MIT), tinydtls (EPL/EDL),
  mongo C/C++ drivers (Apache-2.0), Angular/Clarity (MIT), and the **OpenVPN**
  binary (**GPL-2.0**), plus everything in a Yocto image (many licenses).
  **You are responsible for third-party license compliance when you
  redistribute.** In particular, OpenVPN is invoked as a separate process (not
  linked into our binaries), but shipping an image that contains it carries
  GPL-2.0 obligations for that component. A full bill-of-materials / SBOM is not
  yet generated automatically (see §7).

---

## 7. Limitations & boundaries (read this)

We'd rather you know up front. Current, honest limits of the platform:

**Protocol scope**
- **LwM2M 1.1.1 only.** No LwM2M 1.0 compatibility shim and no 1.2 features
  (binding decision D1).
- **DTLS uses PSK** as the supported, tested production path. The LwM2M
  Raw-Public-Key and X.509-certificate security modes exist in the enum but are
  **not** the supported/hardened path. (X.509 *is* used — but for the OpenVPN
  tunnel, not for LwM2M DTLS.)
- **Notify is Non-Confirmable by default** (every 10th promoted to CON, D4) —
  intentional for efficiency, but means most notifications are best-effort, not
  guaranteed-delivered.

**Topology & scale**
- **Single LwM2M server in v1.** State is keyed by Short Server ID so multi-server
  is a forward-compatible upgrade, but there is **no multi-server, clustering, or
  HA/failover today** (D2). The cloud is a **single-VM reference deployment**
  (the VPN concentrator is co-located with the DM; a split topology is supported
  by override but isn't the default).
- **Device-UI reverse-proxy fan-out is bounded by the published proxy-port
  range** (default `10000–10050` ≈ 50 device UIs per cloud, one host proxy per
  port). Larger fleets need a wider range or host networking. The LwM2M/VPN
  control plane itself is not bound by this number, but "Launch UI" reach is.
- No built-in horizontal scaling, load balancing, or geo-distribution.

**Updates**
- **OTA covers userspace today:** single `.ipk` *or* a `.tar.gz` bundle of the
  whole `iot-*` userspace, via LwM2M Object 5 + the cloud feed (multi-select
  push to a list of endpoints, VPN-independent download with retry/resume,
  re-pushable per campaign, device self-update). The inotify-triggered installer
  (`iot-ota-stage` + `iot-swupdate`) **is shipped**
  (`apps/docs/tdd-yocto-swupdate.md`). **Atomic full-image / A-B partition /
  rollback OTA (RAUC, behind `IOT_AB=1`) — build + bootloader wiring is done but
  pending hardware acceptance** (`apps/docs/tdd-ab-image-ota.md`).

**Hardware & platform**
- **Reference target is the Raspberry Pi 3B** (Yocto), with qemuarm64/ARMv7
  builds. Other boards need their own BSP/recipe work.
- **Images are architecture-specific** — built per-arch in CI (e.g. amd64 cloud,
  arm64 device). Do not cross-ship an image to a different architecture.
- Linux-only device agent (uses nftables, systemd, openvpn, wpa_supplicant, etc.).

**Operational / maturity**
- **No SLA, no commercial support contract, no warranty** — MIT "as is". Best-effort,
  community/author basis.
- **Not independently security-audited or certified** (no Common Criteria, FIPS,
  etc.). Suitable for evaluation, pilots, and self-supported production; harden
  and audit before regulated use.
- **No automated SBOM / license-scan** output yet; compliance review is on you.
- **MongoDB registry mirror** is built but **not activated by default** (in-memory
  registry + async mirror design); the cloud build compiles it out.
- **Interop:** verified our-client ↔ Eclipse Leshan **server**. The reverse
  (Leshan client ↔ our server) is deferred (no canonical Leshan-client image).
- **Observability:** live logs + per-daemon telemetry are provided; there is no
  built-in metrics/export to Prometheus/Grafana or alerting out of the box.

---

## 8. Roadmap (indicative, not committed)

- Full-image / A-B-partition OTA with rollback (Phase 2) + the Yocto
  inotify-triggered installer.
- Multi-server / HA cloud topology.
- Optional X.509/RPK security mode for LwM2M DTLS.
- Automated SBOM + license report.
- Broader board support.

---

## 9. Get started

- Product & architecture: this file + [`README.md`](README.md)
- Deploy walkthrough: [`DEPLOY.md`](DEPLOY.md)
- Cloud internals: [`apps/cloud/CLAUDE.md`](apps/cloud/CLAUDE.md)
- Yocto / device image: [`yocto/meta-iot/README.md`](yocto/meta-iot/README.md)

**Contact / maintainer:** Mohd Naushad Ahmed · naushad.dln@gmail.com ·
<https://github.com/naushada/iot>

*This document describes current capabilities and known limitations in good
faith; it is informational and not a warranty or contractual commitment. The
software is provided under the MIT License "as is".*
