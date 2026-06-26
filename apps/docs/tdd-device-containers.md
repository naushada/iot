# TDD: Shim-Based Container Runtime on the Device

Status: PLAN (not yet implemented)
Author: naushada
Date: 2026-06-22

## 1. Goal

Let an operator pull an OCI/Docker image onto the device from the device-ui,
mount its layers, and run / stop a single container with a custom
CMD/Entrypoint and resource caps — using a **thin shim daemon over `crun`**
(not Docker/Podman) so the footprint fits a 1 GB RPi3B on Yocto.

## 2. Locked design decisions

| Topic | Decision |
|-------|----------|
| Runtime | Ship `crun` (small C OCI runtime); we write `iot-containerd`, a thin shim that pulls, mounts overlay, generates the OCI bundle, and drives `crun`. |
| Image source | OCI / Docker **registry v2 pull** (`registry/repo:tag` + optional user/pass). |
| Networking | **Host network** (container shares device netns; no veth/bridge/NAT). |
| Scope (v1) | **Single** container, manual pull/run/stop. No autostart / reboot persistence. |
| Storage | Images+layers **persist** in `/var/lib/iot-containers`; runtime overlay+bundle **ephemeral** in `/run/iot-containers`. (Dedicated top-level paths — NOT under `/var/lib/iot`, which is ds-server's DynamicUser `StateDirectory=iot` and would get migrated to `/var/lib/private/iot`.) |
| crun source | Upstream **meta-virtualization** layer → `IMAGE_INSTALL += crun`. |
| Control plane | **ds keys** (UI `db/set` command keys, long-polls status keys); `iot-containerd` watches ds. Matches sensord/OTA pattern. |
| Resource limits | **Yes** — mem + cpu caps in the run form → crun cgroup config. |

## 3. Components

```
device-ui /containers page (Angular, Admin-only)
   │  POST /api/v1/db/set   (command keys)
   │  long-poll /api/v1/db/get (status keys)
   ▼
data-store (ds)  ── command/status bus
   ▲
   │  watch()/set()
iot-containerd  (NEW, root daemon, ACE_Reactor)
   ├─ registry v2 client (libcurl): auth → manifest/index → config + layer blobs (sha256 verify)
   ├─ layer store + OCI-whiteout-aware extraction → overlayfs mount
   ├─ OCI bundle gen (config.json: rootfs, env, cmd/entrypoint, host-net, cgroup limits)
   └─ crun create/start/state/kill/delete  + supervision
         ▼
       crun  (meta-virtualization .ipk) — namespaces / cgroups / exec
```

## 4. Data-store keys

Command keys (UI writes via `/api/v1/db/set`):

| Key | Meaning |
|-----|---------|
| `container.image.ref` | e.g. `docker.io/library/nginx:latest` |
| `container.registry.user` / `container.registry.pass` | optional auth (**write-only / sensitive**, never echoed back — follow PSK-provisioning pattern) |
| `container.pull.request` | bump to trigger pull (monotonic token) |
| `container.entrypoint` | override Entrypoint (JSON array or string; empty = image default) |
| `container.cmd` | override CMD (JSON array or string; empty = image default) |
| `container.limit.mem` | e.g. `256M` |
| `container.limit.cpus` | e.g. `0.5` |
| `container.run.request` | bump to trigger create+start |
| `container.stop.request` | bump to trigger kill+delete |

Status keys (`iot-containerd` publishes; volatile; UI long-polls):

| Key | Meaning |
|-----|---------|
| `container.state` | `idle\|pulling\|pulled\|mounting\|created\|running\|stopped\|error` |
| `container.pull.progress` | 0..100 (bytes across all layers) |
| `container.pull.detail` | current layer digest / message |
| `container.image.id` | resolved config digest |
| `container.image.size` | total bytes |
| `container.run.pid` | crun container PID |
| `container.run.started` | start timestamp |
| `container.exit.code` | last exit code |
| `container.status` | human summary mirroring `crun state` |
| `container.error` | last error message |

(New domain ⇒ new keys justified; still reuse `to_int32`/`Value` helpers and the
existing `db/set` + long-poll transport — no new REST endpoints.)

## 5. iot-containerd internals

ACE_Reactor daemon, `data_store::Client` connected to the ds socket, `watch()`
on the command keys. State machine:

1. **Pull** (`container.pull.request` bumped)
   - Parse ref → registry host/repo/tag. `docker.io` ⇒ `registry-1.docker.io`,
     token from `auth.docker.io` on `401 WWW-Authenticate: Bearer`.
   - `GET /v2/<repo>/manifests/<tag>` (Accept: OCI index + docker manifest-list +
     manifest v2). If index/list ⇒ select platform matching device arch
     (`aarch64`→`linux/arm64`, else `linux/arm`/`v7`); re-GET platform manifest.
   - Fetch config blob (image config JSON: Env, Entrypoint, Cmd, WorkingDir, User).
   - Fetch each layer blob by digest → **sha256 verify** (mind the OTA `\n`-in-sha
     bug: trim digests) → store `/var/lib/iot-containers/blobs/sha256/<digest>`.
   - Publish per-byte `container.pull.progress` / `.detail`. End ⇒ `state=pulled`.
2. **Mount** (begins on run)
   - Extract each layer tar (gzip) into `/var/lib/iot-containers/layers/<digest>/`,
     **converting OCI whiteouts** (`.wh.<name>` ⇒ overlay whiteout char dev 0:0;
     `.wh..wh..opq` ⇒ `trusted.overlay.opaque=y` xattr). Cache per-digest.
   - `mount -t overlay` with `lowerdir=<topLayer>:...:<baseLayer>`,
     `upperdir=/run/iot-containers/<id>/upper`,
     `workdir=/run/iot-containers/<id>/work` ⇒ merged
     `/run/iot-containers/<id>/rootfs`. (Use ACE/`ACE_OS` mount wrappers; root daemon.)
3. **Bundle** — generate `/run/iot-containers/<id>/config.json` (OCI runtime spec):
   - `root.path` = merged rootfs; `process.args` = entrypoint+cmd (override or image
     default); `process.env`/`cwd`/`user` from image config + overrides.
   - **Host network** ⇒ omit a `network` namespace (keep mount/pid/uts/ipc; drop
     privileged caps; default seccomp profile).
   - `linux.resources.memory.limit` and `cpu.quota/period` from limit keys.
4. **Run** — `crun create <id> --bundle <dir>` → `state=created` →
   `crun start <id>` → `state=running`, publish pid/started. Supervise via
   reactor (SIGCHLD handler / `crun state` poll timer); on exit publish
   `exit.code`, `state=stopped`.
5. **Stop** (`container.stop.request`) — `crun kill <id> SIGTERM` (grace) →
   `SIGKILL` → `crun delete <id>`; unmount overlay; `state=stopped`.

## 6. Yocto / image prerequisites (Phase 0 gating)

- Add **meta-virtualization** (+ its **meta-filesystems** dep) to *both* build
  paths — they are kept in sync: the kas path (`yocto/kas-iot.yml`) and the
  primary Containerfile/entrypoint path (`yocto/Containerfile` clone +
  `yocto/entrypoint.sh` `bitbake-layers add-layer`). `crun` is pulled via
  `RDEPENDS:iot-containerd` and listed in `packagegroup-iot-full`.
- **Kernel config fragment** `linux-raspberrypi_%.bbappend` + `files/container.cfg`:
  `CONFIG_OVERLAY_FS`, `CONFIG_NAMESPACES` + `PID/NET/UTS/IPC/USER_NS`,
  `CONFIG_MEMCG` + `CONFIG_CGROUP_*`/`CFS_BANDWIDTH` (cpu caps), `CONFIG_SECCOMP`.
  Most are already in the raspberrypi3-64 defconfig; the fragment is a no-op
  where already set. (`seccomp` is already in scarthgap poky's default
  `DISTRO_FEATURES`, so crun builds with seccomp support without a distro change.)
- `tmpfiles.d` (`iot.conf`) creates `/var/lib/iot-containers` and
  `/run/iot-containers`, both `0700 root:root`. **Dedicated top-level paths** —
  NOT under `/var/lib/iot` (ds-server's DynamicUser `StateDirectory=iot`, which
  systemd migrates to `/var/lib/private/iot`) and NOT `RuntimeDirectory=iot` in
  the unit (would clobber the ds socket).
- `iot-containerd.service`: `Type=simple`, root, `After=iot-ds.service
  network-online.target`, `Restart=on-failure`. Add `enable iot-containerd.service`
  to `90-iot.preset` (else `preset-all` disables it — known preset gotcha).
- Recipe `iot_git.bb`: new `-DCONTAINERD_BUILD_DAEMON=ON`, package split
  `FILES:${PN}-containerd` (binary + unit), `SYSTEMD_SERVICE`, SRC_URI for the unit.
- Module layout: `modules/containers/{daemon,inc,src,test}/` + `CMakeLists.txt`,
  linking `datastore_client`, ACE, libcurl.

## 7. device-ui — /containers page

`iot-ui/src/app/containers/containers.component.{ts,html}` (Admin-only; gate behind
role like the device-shell terminal feature). Talks via `HttpsvcService.dbSet` /
status long-poll only.

- **Pull form** (`.form-grid`, 4-col, pad short rows): image ref, optional
  registry user/pass. "Pull" ⇒ `db/set` ref + creds + bump `pull.request`.
  Progress bar bound to `container.pull.progress`/`.detail`. On `pulled` show
  `image.id`/`image.size`.
- **Run form**: Entrypoint, CMD, mem limit, cpu limit. "Run" ⇒ `db/set` overrides
  + bump `run.request`.
- **Status panel**: `container.state`, pid, started, status string; **Stop** button
  ⇒ bump `stop.request`. Any list view uses `clr-datagrid` (never `<table>`).

## 8. Phasing

- **P0 ✅ DONE** Yocto: meta-virtualization+crun (both build paths), kernel fragment, tmpfiles, unit+preset, recipe split, module builds.
- **P1 ✅ DONE** `iot-containerd` skeleton: ds connect/watch, command→reactor dispatch, `container.state` lifecycle.
- **P2 ✅ DONE** Registry v2 pull: ref parse, bearer auth, manifest/index arch-select, blob download + sha256-verify (cache), worker thread + progress → `state=pulled`. gtest: ref parse, auth-challenge parse, token parse, index arch select, digest validation.
- **P3 ✅ DONE** Layer extraction (zlib gzip + in-process tar) + OCI whiteout conversion + path-traversal guard + overlay mount; `run`→extract+mount→`created`, `stop`→unmount. gtest: tar header/octal, whiteout classify, path safety, lowerdir ordering. (crun create/start is P4 — `created` = rootfs assembled, not executing.)
- **P4 ✅ DONE** OCI bundle gen + crun create/start/state/kill/delete + supervision (poll-loop, TERM→grace→KILL, daemon-shutdown kill) + **cgroup mem/cpu limits** (folded in from P5). gtest: image-config parse, args resolution, mem/cpu/user parse, config.json (host-net/args/limits). `run`→running+pid, `stop`→stopped.
- **P5 ✅ DONE (merged into P4)** cgroup mem/cpu limits wiring — `container.limit.mem`/`.cpus` → crun `linux.resources`.
- **P6 ✅ DONE** device-ui **Containers** page (`iot-ui/src/app/containers/`): Admin-gated nav entry; status datagrid (state badge / image / size / pid / pull progress / error); pull form (ref + optional write-only creds); run form (Entrypoint / CMD / mem / cpu) + Run/Stop. Live via a 2s `dbGet` self-poll; commands via `dbSet` + bumped `*.request` tokens. (Did NOT extend the shared `/status` long-poll — avoids a C++ handler change for a page only open while managing a container.)
- **P7** e2e on RPi3B: pull `busybox`/`nginx`, run, verify, stop. Full recipe in §12. **Cgroup driver risk:** crun uses `--cgroup-manager cgroupfs` as root under the unit's `Delegate=yes` subtree — validate mem/cpu limits actually apply on the RPi cgroup-v2 hierarchy. Also validate `/var/lib/iot-containers` is writable + has SD space on the target image.

## 9. Testing

- Host-buildable gtest (podman runner per project memory) for all parser/codegen
  units — no hardware needed.
- e2e on HW for pull→run→stop.

## 10. Security notes

- Registry creds: **write-only sensitive** ds keys, never echoed back to UI.
- Mandatory **sha256 verification** of every blob (digest-trim — OTA `\n` lesson).
- Container runs **host-net** ⇒ can reach device services; mitigate with default
  seccomp profile, dropped capabilities (no `privileged`), and **Admin-only** UI.
- Resource caps mandatory to prevent OOM on the 1 GB device.

## 11. Open items / deferred

- Multi-container, autostart/restart-policy, volume mounts, image GC/pruning,
  registry mirror/offline tar import, container **port mapping** (DNAT) — all
  **post-v1**.
- Confirm RPi3B kernel already enables USER_NS + cgroup v2 hierarchy crun expects.

## 11a. Bridge networking (own container IP) — IMPLEMENTED

Opt-in alternative to host networking, selected per-run via `container.net.mode`
(`host` default | `bridge`). In bridge mode the container gets its **own IP**:
- `generate_oci_config` adds a `network` namespace (own netns) instead of
  omitting it.
- `net_bridge` (daemon, root, via `ip`/`nft`): ensures a host bridge `iot-cni0`
  (gateway `10.88.0.1`), enables IPv4 forwarding, installs a **scoped**
  `inet iot_containers` nft table (postrouting masquerade for the subnet — never
  touches net-router's `iot_router`), then creates a veth pair, attaches the
  host end to the bridge and moves the peer into the container netns as `eth0`
  with `10.88.0.2/24` + default route via the gateway. `crun create` → read pid
  → `bridge_up(pid)` → publish `container.net.ip`/`.gateway` → `crun start`; teardown
  on exit (`bridge_down` deletes the veth; the bridge + table persist).
- Subnet overridable via `container.net.subnet` (a single /24; default
  `10.88.0.0/24`). Single container ⇒ static `.2` (no IPAM).
- UI: a **Network** select in the run form + a **Network**/**IP** row in the
  status datagrid (shows `10.88.0.2` when bridge + running).
- Kernel: `container.cfg` adds `BRIDGE`/`VETH`/`BRIDGE_NETFILTER`/`NF_NAT`.
  `RDEPENDS` adds `iproute2`/`nftables`.
- **Deferred within this:** container port mapping (host:port → container IP via
  DNAT) and multi-container IPAM.

### Bridge mode HW e2e (extends §12)

```sh
ds-cli set container.net.mode '"bridge"'
ds-cli set container.image.ref '"docker.io/library/busybox:latest"'
ds-cli set container.pull.request "$(date +%s)" ; ds-cli watch --count=30 container.state
ds-cli set container.cmd '"sleep 600"'
ds-cli set container.run.request "$(date +%s)" ; ds-cli watch --count=10 container.state
ds-cli get container.net.ip container.net.gateway          # 10.88.0.2 / 10.88.0.1
ip addr show iot-cni0                                       # bridge up, 10.88.0.1/24
nft list table inet iot_containers                         # masquerade rule present
crun --root /run/iot-containers/state exec c0 /bin/sh -c \
  'ip addr show eth0; ping -c1 10.88.0.1; wget -qO- http://example.com | head -1'
ds-cli set container.stop.request "$(date +%s)"            # veth torn down, net.ip cleared
```

## 12. Phase 7 — hardware test recipe (RPi3B)

End-to-end validation on a real device. Assumes an image built from this branch
(crun + iot-containerd) and shell access. Device access per the project memory:
`ssh root@<device-ip>` (LAN), device-ui at `http://<device-ip>:8080`
(admin/admin). Control via `ds-cli` on the device (string values are JSON, so
double-quote them): `ds-cli set <key> '"<val>"'`, `ds-cli get <key>`,
`ds-cli watch --count=N <key>...`. The single-container id is `c0`; crun state
lives under `/run/iot-containers/state`.

### 12.0 Pre-flight — image + kernel + daemon

```sh
opkg list-installed | grep -E 'iot-containerd|crun'   # both present
crun --version
systemctl status iot-containerd                       # active (running)
journalctl -u iot-containerd -n 5                      # "up: ... (pull/run/stop ready)"

# storage dirs (tmpfiles), writable, on a writable fs with space
ls -ld /var/lib/iot-containers /run/iot-containers
df -h /var/lib

# kernel features
grep -q overlay /proc/filesystems && echo "overlayfs OK"
ls /sys/fs/cgroup/cgroup.controllers                  # cgroup v2; lists "memory cpu ..."
ls -l /proc/self/ns                                   # net/pid/mnt/uts/ipc/user present
zcat /proc/config.gz 2>/dev/null | grep -E \
  'CONFIG_(OVERLAY_FS|USER_NS|NET_NS|MEMCG|CFS_BANDWIDTH|SECCOMP)='

ds-cli get container.state                            # idle
```

If the `memory`/`cpu` controllers are missing from a child cgroup, enable them on
the parent: `echo +memory +cpu > /sys/fs/cgroup/cgroup.subtree_control`.

### 12.1 Pull (busybox)

```sh
ds-cli set container.image.ref '"docker.io/library/busybox:latest"'
ds-cli set container.pull.request "$(date +%s)"
ds-cli watch --count=30 container.state container.pull.progress   # pulling → pulled
ds-cli get container.state container.image.id container.image.size container.error
```

Expect `state=pulled`, `image.id=sha256:…`, `error` empty. Verify the store +
that a blob's contents hash to its filename (the sha256-verify path):

```sh
ls /var/lib/iot-containers/blobs/sha256/ /var/lib/iot-containers/manifests/
f=$(ls /var/lib/iot-containers/blobs/sha256/ | head -1)
sha256sum /var/lib/iot-containers/blobs/sha256/$f      # digest == $f
```

### 12.2 Run + verify (busybox)

```sh
ds-cli set container.cmd '"sleep 600"'                 # ws-split → ["sleep","600"]
ds-cli set container.run.request "$(date +%s)"
ds-cli watch --count=10 container.state                # mounting → created → running
ds-cli get container.state container.run.pid

crun --root /run/iot-containers/state list             # c0  running
mount | grep /run/iot-containers/c0/rootfs             # overlay mounted
sed -n '1,5p' /run/iot-containers/c0/config.json       # generated bundle

# exec in: confirm rootfs, host networking (shared device IP), DNS
crun --root /run/iot-containers/state exec c0 /bin/sh -c \
  'id; ip addr 2>/dev/null | grep "inet "; cat /etc/resolv.conf; \
   wget -qO- http://example.com 2>/dev/null | head -1'
```

Host networking means `ip addr` inside the container shows the **device's**
interfaces/IP — there is no separate container IP (expected for v1).

### 12.3 Resource limits (cgroups)

```sh
ds-cli set container.stop.request "$(date +%s)" ; sleep 3
ds-cli set container.limit.mem '"64M"'
ds-cli set container.limit.cpus '"0.5"'
ds-cli set container.run.request "$(date +%s)" ; sleep 3
cat /sys/fs/cgroup/c0/memory.max                       # 67108864
cat /sys/fs/cgroup/c0/cpu.max                          # "50000 100000"
```

(If `/sys/fs/cgroup/c0` isn't the path, find it: `grep -r '' \
/sys/fs/cgroup/*/cgroup.procs 2>/dev/null | grep "$(ds-cli get container.run.pid)"`.)

### 12.4 Stop

```sh
ds-cli set container.stop.request "$(date +%s)"
ds-cli watch --count=6 container.state                 # stopping → stopped
crun --root /run/iot-containers/state list             # empty
mount | grep /run/iot-containers/c0 || echo "unmounted OK"
```

### 12.5 nginx (host-net port bind)

```sh
ds-cli set container.image.ref '"docker.io/library/nginx:stable"'
ds-cli set container.pull.request "$(date +%s)" ; ds-cli watch --count=40 container.state
ds-cli set container.entrypoint '""' ; ds-cli set container.cmd '""'   # image defaults
ds-cli set container.run.request "$(date +%s)" ; ds-cli watch --count=10 container.state
curl -s http://127.0.0.1:80/ | head -1                 # nginx serves on the device (host net)
ds-cli set container.stop.request "$(date +%s)"
```

(nginx binds `:80` on the device via host net — confirm nothing else owns `:80`.)

### 12.6 device-ui path

Browse `http://<device-ip>:8080` → login (admin) → **Containers** (Admin-only).
Pull form → `busybox:latest` → **Pull** (progress bar → pulled). Run form → CMD
`sleep 600`, Memory `64M` → **Run** (badge → running, PID shown) → **Stop**.

### 12.7 Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| pull `registry unreachable` / TLS | clock unset (no-RTC) → NTP not synced; `date`, check timesyncd |
| pull `401` | private image → set registry user/pass |
| `image has no Entrypoint/Cmd` | set `container.cmd` (busybox has no default CMD in some tags) |
| `crun create failed` cgroup | enable controllers in parent `cgroup.subtree_control` (see 12.0) |
| `overlay mount failed` EINVAL | kernel missing `OVERLAY_FS`, or `/var`/`/run` fs quirk → `dmesg` |
| DNS fails in container | host `/etc/resolv.conf` empty (host-net reuses it) |

### 12.8 Cleanup

```sh
ds-cli set container.stop.request "$(date +%s)"
rm -rf /var/lib/iot-containers/blobs /var/lib/iot-containers/layers \
       /var/lib/iot-containers/manifests   # drop cached images
```

---

## 13. Phase 2 — multiple containers (dynamic named)

v1 runs **one** container (`kContainerId="c0"`, singular `container.*` keys). Phase 2
lets an operator run **any number** of independently-named containers, each with its
own bridge IP, shown one-per-row in a device-ui grid.

### 13.1 Why a JSON document, not `container.<name>.*` keys

The ds schema is **static** — keys are registered up-front in `container.lua`; there
is no mechanism to register `container.web.state`, `container.db.state`, … at
runtime. So multiplicity lives in **values, not key names** — exactly the pattern the
cloud already uses for `cloud.endpoint.credentials` (a JSON array of per-endpoint
records). Two new key groups (schema added):

**Status (daemon → UI), volatile:**
- `container.instances` — JSON array, one object per container:
  `{name, image, imageId, size, state, ip, gateway, pid, exitCode, mem, cpus, net, started, error}`.
  Republished on every state change; the UI grid renders it directly.

**Command envelope (UI → daemon):** set the fields, then bump `container.cmd.request`:
- `container.cmd.name`  — target container (the row's name; the create key for a new one)
- `container.cmd.action` — `pull | run | stop | remove`
- `container.cmd.image | .entrypoint | .cmd | .mem | .cpus | .net | .subnet` — run params

One envelope + one monotonic token = the same baseline-at-startup, no-fire-on-boot
semantics as the v1 `*.request` keys, and only one command is in flight at a time
(a registry pull is the long pole; serialising is fine for an edge gateway).

### 13.2 Daemon

Refactor the per-container state out of `Containerd`'s members into a
`ContainerInstance` struct — its own `state`, request flags, `pull`/`run` threads,
`merged` rootfs path, crun id, bridge IP — and hold
`std::map<std::string, std::unique_ptr<ContainerInstance>> m_containers` keyed by
name. `on_command_event` parses the envelope, finds/creates the named instance, and
dispatches `pull/run/stop/remove` to it. `publish_instances()` serialises the map to
`container.instances`.

- **crun id / paths** per name: `id = "c-"+sanitize(name)`, overlay/bundle under
  `/run/iot-containers/<id>/`, image store stays shared in `/var/lib/iot-containers`.
- **IP allocation**: a small allocator hands out `.2, .3, .4…` in the bridge /24,
  tracking in-use IPs across running instances; freed on stop/remove. (v1's bridge
  code hardcodes `.2` — generalise to "next free".)
- **remove**: stop if running, then drop the instance from the map + republish.

### 13.3 UI

Replace the single-container key-value card with a `clr-datagrid`:

| Name | Image | State | IP | Mem | CPU | Net | Actions |
|------|-------|-------|----|-----|-----|-----|---------|

Rows = parsed `container.instances`. **IP column** shows `ip` (bridge mode). An *Add
container* form (name, image, net, mem, cpu) writes the envelope with `action=pull`
then `run`; per-row **Run / Stop / Remove** buttons write the envelope for that
`name`. Long-polls `container.instances` (+ the in-flight `container.pull.progress`).

### 13.4 Migration / compatibility

The legacy singular `container.*` keys stay registered and mirror the
most-recently-touched instance during rollout, so an old UI keeps working until the
grid ships. Once the grid lands they can be retired.

### 13.5 Phasing

1. **Schema + design** (this) — `container.instances` + `container.cmd.*` registered.
2. **Daemon** — `ContainerInstance` map, envelope dispatch, IP allocator, `publish_instances()`.
3. **UI** — datagrid + Add form + per-row actions.
4. **Tests** — `container_net` IP-allocator unit test; daemon envelope-dispatch test.
