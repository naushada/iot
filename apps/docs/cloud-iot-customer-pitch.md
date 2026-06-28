# cloud-iot — Device Management & Secure Remote Access

*A customer overview. For the engineering capacity study behind the numbers,
see [tdd-cloud-load-benchmark.md](tdd-cloud-load-benchmark.md).*

---

## What it is

cloud-iot is a self-hostable platform for **onboarding, managing, and securely
reaching IoT devices at scale** — built on the open **OMA LwM2M 1.1.1** standard
over **CoAP/DTLS**, so it interoperates with standard LwM2M clients and tooling
(e.g. Leshan), not a proprietary lock-in.

One deployment gives you:

- **Zero-config device onboarding** — a device powers on, bootstraps, and
  registers itself; no per-device cloud clicking.
- **Secure-by-default transport** — every device↔cloud exchange is encrypted
  with **DTLS pre-shared keys**; each device has its own key.
- **Secure remote access** — a per-device **VPN tunnel** plus a built-in
  reverse proxy lets an operator open any device's web UI or shell from the
  cloud console, through NAT/firewalls, with no inbound ports on the device.
- **Over-the-air updates**, **live telemetry** (sensors, GPS, cellular,
  vehicle/OBD-II), and a **web console** with role-based login.

## Why it matters

| Capability | Customer benefit |
|---|---|
| Standards-based (LwM2M/CoAP/DTLS) | No vendor lock-in; reuse off-the-shelf clients |
| Per-device PSK + per-device VPN | Strong isolation; one compromised key ≠ fleet exposure |
| Self-hosted | Your data stays on your infrastructure; no third-party SaaS dependency |
| Runs on a single modest VM | Low operating cost to start; scales with hardware |
| Open device stack | Sensors, cellular/GPS, containers, OTA all included |

## Security posture

- **Encryption in transit:** DTLS-PSK on all device traffic; OpenVPN (TLS) for
  the remote-access tunnel.
- **Per-device identity:** each device authenticates with its own key; keys are
  stored write-only and never returned by the API.
- **Least privilege on the device:** hardware-owning daemons run privileged;
  the application layer runs unprivileged and only reads state.
- **Role-based console:** Admin vs Viewer; remote shell is off by default and
  gated behind Admin.

## Scale & performance

Measured against a single, modest cloud instance (5 vCPU / 8 GB). These are
**conservative lab figures** — a dedicated, larger instance goes further.

- **~1,000 devices onboarding *simultaneously*** complete in **under a second**
  (99th percentile) at ≥99% success.
- **~100 device onboardings per second**, sustained.
- A burst far above that doesn't fail the platform — it **queues gracefully**
  (median onboarding stays ~0.2 s) and drains; devices retry transparently.
- **Steady-state fleet of several thousand devices per instance** is well
  within reach; remote-access (VPN) capacity is the first dimension to size for
  larger fleets and is straightforward configuration.

Headroom is real: CPU stayed under 2% during these tests — capacity grows
predictably with cores and with horizontal scaling on the roadmap.

## Deployment & tenancy — choosing your model

cloud-iot ships as a container stack you run on your own VM (or ours, managed).

**Today — dedicated instance per customer (recommended):**
Each customer gets an isolated cloud instance: its own device registry, its own
VPN subnet and certificate authority, its own console and credentials. This
gives the **strongest possible isolation** — no shared data plane, no
cross-customer blast radius — and is the model we run in production today.

| | Dedicated instance (today) | Shared multi-tenant (roadmap) |
|---|---|---|
| Isolation | Strongest — separate data + network plane | Logical, per-tenant scoping |
| Onboarding a new customer | Spin up a stack | Add a tenant |
| Cost at small scale | One VM per customer | Shared infrastructure |
| Best for | Regulated / high-isolation / per-customer SLAs | Many small fleets on one platform |

**On the roadmap — shared multi-tenant:** a single cloud-iot serving multiple
organizations, with per-tenant device registries, per-tenant VPN subnets and
CAs, and tenant-scoped console logins. This lowers per-customer cost for
operators running **many small fleets**. The capacity headroom above shows a
single instance has ample room to host multiple tenants; the work is in adding
the tenant dimension (isolation boundaries, scoped APIs), not in raw scale.

> Practical guidance: start with a **dedicated instance per customer** for
> isolation and simplicity. Move to shared multi-tenant when you're operating
> enough independent fleets that per-customer VMs dominate cost.

## What's included on the device

OTA updates · live sensor/GPS/cellular telemetry · vehicle (CAN/OBD-II)
telemetry · on-device container runtime · remote web UI + shell · automatic
network/Wi-Fi/cellular bring-up · secure factory provisioning.

## Roadmap highlights

- Shared multi-tenant cloud (above)
- Full-image OTA (in addition to package updates)
- Zero-touch fleet onboarding from a single master secret (in qualification)
- Horizontal scaling of the device-facing plane beyond a single instance

---

*Capacity figures are from an internal load benchmark; method, raw results, and
caveats are documented in
[tdd-cloud-load-benchmark.md](tdd-cloud-load-benchmark.md). Figures are
conservative (load generator and cloud shared one host) and improve with
dedicated, larger hardware.*
