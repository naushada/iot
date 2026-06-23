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
- **P7** e2e on RPi3B: pull `busybox`/`nginx`, run, verify, stop. **Cgroup driver risk:** crun uses `--cgroup-manager cgroupfs` as root under the unit's `Delegate=yes` subtree — validate mem/cpu limits actually apply on the RPi cgroup-v2 hierarchy. Also validate `/var/lib-containers` is writable + has SD space on the target image.

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

- Multi-container, autostart/restart-policy, bridge networking, volume mounts,
  image GC/pruning, registry mirror/offline tar import — all **post-v1**.
- Confirm RPi3B kernel already enables USER_NS + cgroup v2 hierarchy crun expects.
