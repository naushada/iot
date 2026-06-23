# containers — on-device single-container runtime (iot-containerd)

A thin, **crun-backed** shim that lets an operator pull an OCI/Docker image
onto the device from the device-ui, mount its layers, and run / stop a single
container with a custom CMD/Entrypoint and resource caps. Designed for a 1 GB
RPi3B on Yocto — no Docker/Podman daemon. Full design:
[`apps/docs/tdd-device-containers.md`](../../apps/docs/tdd-device-containers.md).

## Layout

| Path | What |
|------|------|
| `inc/image_ref.hpp`, `src/image_ref.cpp` | Pure OCI/Docker image-reference parsing — host-unit-testable `containers_core`. |
| `inc/registry.hpp`, `src/registry.cpp` | Pure registry-v2 protocol logic: Bearer-challenge + token + manifest/index parse (nlohmann), platform select, digest validation. Part of `containers_core`. |
| `inc/oci_layer.hpp`, `src/oci_layer.cpp` | Pure tar-header parse, OCI-whiteout classification, path-traversal guard, overlay lowerdir ordering. Part of `containers_core`. |
| `inc/oci_bundle.hpp`, `src/oci_bundle.cpp` | Pure image-config parse, CMD/Entrypoint resolution, mem/cpu/user parsing, and OCI runtime `config.json` generation (host-net, default caps, cgroup limits). Part of `containers_core`. |
| `daemon/http_client.{hpp,cpp}` | HTTP(S) transport — `curl` via `ACE_Process` + OpenSSL streaming SHA-256 + `mkdir_p`. |
| `daemon/registry_puller.{hpp,cpp}` | Pull orchestration: auth → arch-select → download + sha256-verify blobs to the store. |
| `daemon/layer_store.{hpp,cpp}` | gzip+tar layer extraction (zlib) with whiteout → overlay conversion + cache, and the overlayfs `mount(2)` that assembles the rootfs. |
| `daemon/crun_runtime.{hpp,cpp}` | `crun` CLI wrapper (create/start/state/kill/delete) via `ACE_Process`. |
| `daemon/containerd.{hpp,cpp}`, `daemon/main.cpp` | `iot-containerd` — privileged, reactor-driven (ACE-only) shim. Watches `container.*` commands, runs pull + (mount→bundle→crun→supervise) on worker threads, publishes status. |
| `schemas/container.lua` | The `container.*` data-store schema (operator + daemon contract). |
| `test/*_test.cpp` | gtest for the pure core (image-ref, registry, oci-layer, oci-bundle). |

## Control plane (data-store keys)

The device-ui writes command keys via `/api/v1/db/set` and long-polls the
status keys; `iot-containerd` watches the `*.request` tokens and publishes
`container.state` / progress. See `schemas/container.lua` for the full list.

## Status: phased

This module lands incrementally (see the TDD doc):

- **Phase 1 (done): control-plane skeleton.** ds connect, `container.state`
  lifecycle, command watch + reactor dispatch.
- **Phase 2 (done): registry-v2 pull.** `pull` request → Bearer auth →
  manifest/index fetch + arch-select for this device → download config + layer
  blobs to `/var/lib/iot-containers/blobs/sha256/`, each **sha256-verified**
  (cached blobs reused), with `container.pull.progress`/`.detail` updates,
  ending at `container.state=pulled` (`container.image.id` / `.size` set). Runs
  on a worker thread so the reactor stays responsive. The platform image
  manifest is persisted under `/var/lib/iot-containers/manifests/` for the mount
  phase. Registry credentials are consumed write-only (cleared from ds).
- **Phase 3 (done): layer extraction + overlay mount.** A `run` extracts each
  layer (zlib gzip + in-process tar) into `/var/lib/iot-containers/layers/<hex>/fs`
  — converting OCI whiteouts (`.wh.*` → char dev 0:0; `.wh..wh..opq` →
  `trusted.overlay.opaque`), guarding traversal, preserving mode/uid/gid — then
  `mount(2)`s an overlay (lowers base→top reversed, ephemeral upper/work under
  `/run/iot-containers/c0`) to assemble the rootfs.
- **Phase 4 (done): crun lifecycle + supervision.** After mount, the run worker
  reads the image config, resolves args (image Entrypoint/Cmd + UI overrides,
  Docker semantics), generates the OCI `config.json` (host networking, default
  caps, NoNewPrivileges, mem/cpu cgroup limits, `/etc/resolv.conf` bind), and
  drives `crun create`→`start`→`state` → `container.state=running` with
  `run.pid`. It then supervises (polls `crun state`) until the container exits;
  `stop` requests SIGTERM→grace→SIGKILL, then `crun delete` + unmount →
  `stopped`. Daemon shutdown kills + cleans up.
- **Phase 6 (done): device-ui Containers page.** `iot-ui/src/app/containers/` —
  Admin-only nav entry with a status datagrid (state badge / image / size / pid /
  pull progress / error), a pull form (image ref + optional write-only registry
  creds), and a run form (Entrypoint / CMD / mem / cpu) with Run/Stop. Drives the
  `container.*` keys: a 2s `dbGet` self-poll for live status, `dbSet` + bumped
  `*.request` tokens for commands.
- **Bridge networking (own IP):** opt-in `container.net.mode=bridge` gives the
  container its own IP via a veth into a host bridge (`iot-cni0`,
  `10.88.0.0/24`) with masqueraded egress (scoped `inet iot_containers` nft
  table). `container_net` (core) plans the subnet + nft ruleset;
  `net_bridge` (daemon) does the `ip`/`nft` plumbing; the UI adds a Network
  select + IP row. Host networking stays the default. See TDD §11a.
- **Phase 7 (next)**: e2e on RPi3B hardware (pull → run → stop, host + bridge),
  validating overlayfs + crun + cgroups + veth/masquerade on the real kernel.

## Build

Daemon is built in the integrated iot image (`-DCONTAINERS_BUILD_DAEMON=ON`).
For a host core/test build:

```sh
cmake -S modules/containers -B build/containers
cmake --build build/containers
ctest --test-dir build/containers
```

## Runtime

`iot-containerd` runs as **root** (it will mount overlayfs and drive crun's
namespaces/cgroups). Storage uses dedicated top-level paths (created by
tmpfiles.d): persistent images/layers under `/var/lib/iot-containers`,
ephemeral overlay/bundle under `/run/iot-containers`. These are deliberately
NOT under `/var/lib/iot` — that path is ds-server's DynamicUser
`StateDirectory=iot` and systemd migrates pre-created children into
`/var/lib/private/iot`. `crun` ships from the meta-virtualization layer.
