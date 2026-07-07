# TDD — Yocto swupdate: inotify-triggered local IPK installer

Status: **DESIGN (for review)** · Target: Yocto/RPi (systemd) only · Author: 2026-06-14

## 1. Goal

Re-shape the device-side OTA *apply* on Yocto/RPi into a **decoupled,
inotify-triggered installer** (a "swupdate module"), per this flow:

1. When an update is kicked in, the `.ipk` is **downloaded/copied into a
   tmpfs spool** (`/run/iot/update/`, wiped on reboot).
2. An **empty trigger file** named `update` is created in that spool.
3. A **swupdate module monitors the trigger via inotify**.
4. On notification, it **installs the `.ipk`(s)** with `opkg`.
5. It **decides reboot-vs-restart itself**: reboot only when a base/kernel
   package landed; otherwise restart the affected iot services and run on the
   new packages immediately.

Scope is **Yocto only**. The docker dev stack updates by image pull and is out
of scope (it has no systemd/opkg); these are recipe-side artifacts under
`yocto/meta-iot/`, exactly like the existing `iot-ota-apply`.

## 2. Why change the current design

Today (`apps/src/main.cpp:687` `ota_launch_apply` → `systemd-run --unit=iot-ota-apply
/usr/bin/iot-ota-apply <url>`, script at
`yocto/meta-iot/recipes-iot/lwm2m/files/iot-ota-apply`) a **single transient
unit** does *download + verify + opkg install + restart* in one shot, launched
by the lwm2m client.

Problems / limits this redesign fixes:

- **No separation of concerns.** Network download (can be slow/flaky) and the
  privileged install run in the same unit; a re-run repeats the download.
- **Single trigger source.** Only the lwm2m client (Object 5 / `iot.update.request`)
  can drive it. There's no clean way to install a locally-staged `.ipk` (USB,
  field tech `scp`, a future delta agent).
- **Reboot is never considered.** The current script only `try-restart`s a
  fixed unit list — a kernel/base-files `.ipk` would be installed but the
  system would keep running the old kernel until a manual reboot.

The new design splits responsibilities at the **spool + trigger** boundary:

```
  download/stage  ─────►  /run/iot/update/*.ipk  +  touch update  ─────►  install
  (network, root)         (tmpfs spool, root-owned)        (inotify)       (opkg, root)
   iot-ota-stage                                       iot-swupdate.path → iot-swupdate.service
```

Anything that can drop an `.ipk` + `touch update` into the spool gets the same
install path — OTA is just one producer.

## 3. Components

### 3.1 Spool directory — `/run/iot/update/` (tmpfs)

- `/run` is tmpfs on the systemd image, so the spool is **wiped on reboot**
  (requirement 1). No stale `.ipk` survives a boot → no re-trigger loop after a
  reboot-required update.
- **But within a single boot, a failed/interrupted campaign can leave stale
  `.ipk`s in the spool** — and `iot-swupdate` globs `$SPOOL/*.ipk`, so mixing two
  package sets makes `opkg install` die on `cannot install both iot-X-1.3.1 and
  iot-X-1.1.0 — conflicting requests`. Field-observed: a v1.1.0 set left behind
  when the watchdog crash-loop killed that OTA mid-flight poisoned the next v1.3.1
  install (`result=9`, device stuck on the old version). So **`iot-ota-stage`
  purges any pre-existing `.ipk`/`.raucb`/bundle (+ stale `update.meta`) right
  after sha-verify**, keeping only the current campaign's download — each OTA
  installs exactly its own packages.
- Created at boot by tmpfiles.d (the recipe already ships
  `/usr/lib/tmpfiles.d/iot.conf`): add
  `d /run/iot/update 0755 root root -`.
- **Root-owned, 0755.** Only root writers (the stager / the installer). The
  lwm2m client (`engineer`) never writes here directly — it asks the root
  stager to (§3.2), preserving the privilege boundary.

Layout during an update:

| Path | Who writes | Meaning |
|------|-----------|---------|
| `/run/iot/update/<pkg>.ipk` | stager | the package(s) to install |
| `/run/iot/update/update.meta` | stager (optional) | `key=value`: `version=`, `sha256=`, `reboot=auto\|always\|never` |
| `/run/iot/update/update` | stager (**last**, atomic) | **empty** trigger file (requirement 2) |
| `/run/iot/update/.work-<pid>/` | installer | private snapshot it installs from |
| `/run/iot/update/last.log` | installer | opkg output of the last run (debug) |

**Ordering contract:** the stager writes every `.ipk` (and `update.meta`) in
full, *then* `touch`es `update` as the final step. The trigger's existence ⇒ a
complete payload is present. The trigger is **empty** by design — all metadata
lives in `update.meta` so the trigger semantics stay "payload ready".

### 3.2 Stager — `/usr/bin/iot-ota-stage` (root, path-triggered)

A slimmed refactor of today's `iot-ota-apply`: it **downloads + verifies +
stages + triggers**, but **does not install**.

- Launched via a **spool trigger**, NOT `systemd-run`. `ota_launch_apply()` in
  `apps/src/main.cpp` runs in the lwm2m client, which is **unprivileged**
  (`User=engineer`) and therefore **cannot** create a system transient unit —
  `systemd-run` returns "Access denied" (polkit; no agent on the busybox image),
  so the original `systemd-run --unit=iot-ota-stage …` silently failed and the
  stager never ran. Instead, `ota_launch_apply()` atomically writes the package
  URL to `/run/iot/update/stage.req` (temp + rename); the **`iot-ota-stage.path`**
  unit (`PathExists`) fires **`iot-ota-stage.service`** (oneshot, **root**), which
  runs `iot-ota-stage` with no argument — it reads the URL from `stage.req` and
  removes the file to re-arm the path unit. `/run/iot/update` is `0775 root:iot`
  (tmpfiles `iot.conf`) and the client has `SupplementaryGroups=iot`, so the
  write needs no privilege. Same engineer→root handoff as the cert-apply →
  `iot-vpn-cert.path` flow. Runs as **root** (network + write the root-owned
  spool); since it no longer installs, it is never killed by opkg replacing a
  running binary.
- Steps:
  1. Parse `?sha256=`, `?version=`, `?reboot=` query params (same parser as
     today).
  2. `iot.update.state = 1` (downloading); `wget`/`curl` the URL to
     `/run/iot/update/<pkg>.ipk` **direct over the public WAN** (the cloud
     resolves a relative `ipk_url` against its public address — `cloud.dm.uri`
     host or `cloud.firmware.base.url` — NOT the VPN tunnel, so OTA still works
     when the tunnel is down and a bundle can safely replace the VPN client
     itself). Retried with backoff + **resume** (`curl -C -` / `wget -c`) up to
     `iot.update.retries` (default 5) so a flaky uplink doesn't fail a campaign;
     integrity is still pinned by the sha256 below, which arrives over the
     trusted DTLS control plane.
  3. Verify sha256 if given → on mismatch `iot.update.result = 5`, state 0, abort
     (no trigger written).
  4. `iot.update.state = 2` (downloaded); write `update.meta`
     (`version`/`sha256`/`reboot`).
  5. **`touch /run/iot/update/update`** (atomic last step) and exit. The install
     is now the .path unit's job.
- Multiple packages in one campaign: the stager may drop several `<pkg>.ipk`
  before the single final `touch` (opkg installs them together, resolving deps).
- **Bundle (`.tar.gz`) — whole-userspace upgrade in one LwM2M push.** When the
  URL basename ends in `.tar.gz`/`.tgz` the artifact is an **iot-* bundle**
  (built by `iot-bundle.bb`: every `iot-*.ipk` tarred flat). The stager
  downloads + sha256-verifies the **tarball**, then `gzip -dc | tar -xf -`
  extracts the `.ipk`s straight into the spool (busybox-safe — no reliance on
  `tar -z`) and drops the tarball, so the existing multi-package path (above)
  installs them all. This is what lets the cloud upgrade a device's full
  userspace — and a whole *list* of devices, since the cloud push already fans a
  single `cloud.update.request{serials:[…]}` out to every endpoint — in one
  shot. Lighter than the RAUC A/B `.raucb` image bundle (no reboot unless a
  base/kernel package lands).

### 3.3 Watcher — `iot-swupdate.path` (systemd, inotify)

A systemd **`.path` unit** — the same proven pattern as the existing
`iot-vpn-cert.path` (which inotify-watches the VPN cert). systemd `.path` units
**use the kernel inotify API internally**, so this *is* the "swupdate module
monitors via inotify" of requirement 3, with zero custom daemon code.

```ini
# iot-swupdate.path
[Unit]
Description=Watch the IPK update spool trigger (inotify)
Documentation=https://github.com/naushada/iot

[Path]
PathExists=/run/iot/update/update
Unit=iot-swupdate.service

[Install]
WantedBy=multi-user.target
```

`PathExists` activates `iot-swupdate.service` as soon as the trigger appears.
A `.path` unit does **not** re-activate until the watched path is gone *and* the
triggered service has finished — so the installer **must delete the trigger** to
re-arm (§3.4), which also prevents a re-trigger loop.

> Alternative considered: a long-running custom inotify daemon (C++/`inotifywait`)
> as the literal "module". Rejected for v1 — on a systemd target the `.path`
> unit is the idiomatic, dependency-ordered, crash-safe inotify mechanism we
> already ship and test (`iot-vpn-cert.path`). The daemon adds a process and
> re-implements what systemd gives us. Easy to swap in later if needed.

### 3.4 Installer — `iot-swupdate.service` → `/usr/bin/iot-swupdate` (root, oneshot)

```ini
# iot-swupdate.service  (no [Install]; activated by the .path unit)
[Unit]
Description=Install staged IPK(s) and restart/reboot as needed
Documentation=https://github.com/naushada/iot
After=iot-ds.service
Wants=iot-ds.service

[Service]
Type=oneshot
ExecStart=/usr/bin/iot-swupdate
```

`/usr/bin/iot-swupdate` algorithm:

1. **Lock** (`flock /run/iot/update/.lock`) — serialize concurrent activations.
2. **Snapshot + re-arm:** make `/run/iot/update/.work-$$/`, **move** all `*.ipk`
   + `update.meta` into it, then **`rm -f /run/iot/update/update`**. Removing the
   trigger here re-arms the `.path` unit; moving the `.ipk`s out means a new
   campaign staged mid-install lands cleanly for the next activation and we never
   install a half-written new file.
3. If no `.ipk` in the work dir → clean up, exit 0 (spurious trigger).
4. **Downgrade guard:** if `update.meta` carries a `version=` and its semver
   (`major.minor.patch`, `+build`/`-pre` ignored) is **strictly older** than the
   running `iot.version`, skip the install: `iot.update.version` = the running
   version, `iot.update.result = 10` (skipped — not newer), state 0, **exit 0**
   (clean, *not* a failed unit). Same-semver re-installs (a newer `+git` build of
   the same release) are allowed. This is the last line of defense — the cloud
   push gate (`apps/src/main.cpp`, see `tdd-installed-version.md` §3b) also
   refuses non-newer versions, but it fails closed only once it has read the
   device's `/3/0/3` version; a push ~2 s after registration once slipped a
   1.2.0 bundle onto a 1.3.0 unit and `opkg` blew up on the cross-version deps.
   A `semv()` shell helper mirrors the cloud-side `semv()` so both gates agree.
5. `iot.update.state = 3` (updating).
6. `opkg install --force-reinstall --force-downgrade <work>/*.ipk`
   `> /run/iot/update/last.log 2>&1`. On failure: `iot.update.result = 9`,
   state 0, log tail, clean work dir, exit 1 (**no reboot, no restart**).
   (`--force-downgrade` remains for the allowed same-semver `+git` reinstall,
   whose opkg version string can read as a downgrade; the step-4 guard already
   blocked any real semver downgrade.)
7. Derive version (`update.meta` `version=` wins, else parsed from opkg log) →
   `iot.update.version`.
8. **Config/schema migration (§11)** — restart `ds-server` so it loads the new
   schemas, then run any pending migrations. On migration failure: `result=9`,
   skip the daemon restart, exit non-zero (operator intervention) — opkg is
   already applied, so we don't restart app daemons against a half-migrated ds.
9. `iot.update.result = 1`; `iot.update.state = 0`.
10. **Reboot decision (§4).**
11. Clean `.work-$$/`.

Result codes (`iot.update.result`): `0` initial, `1` success, `2` rolled-back
(A/B), `5` integrity, `8` uri, `9` install/migration error, `10` skipped
(offered not newer — downgrade refused).

ds writes use `ds-cli --socket=/run/iot/data_store.sock` (root) — identical to
today's script, and ds (`/var/lib/iot`) is persistent so the result survives a
reboot and the relaunched lwm2m client mirrors it back onto Object 5
(`/5/0/3`, `/5/0/5`, `/5/0/7`).

### 3.5 Local upload (drag-and-drop) — second source

Besides the cloud URL (`iot.update.request` → stager), the device-ui
**Software Update** page accepts a drag-and-dropped `.ipk` (or small `.tar.gz`
bundle). iot-httpd's `POST /api/v1/update/upload?name=<f>` (admin-only) writes the
raw body straight into the spool (`/run/iot/update/<f>`) and trips the trigger —
the same `iot-swupdate.path` install path, no stager/download. Yocto/systemd only.

Perms: iot-httpd runs `DynamicUser=yes`, so the spool is `0775 root:iot` and the
unit gets `SupplementaryGroups=iot`; root `iot-swupdate` installs. Bounded by the
HTTP parser's 8 MiB body cap (`kMaxBody`) — larger / full-image bundles need the
streaming upload + A/B image OTA in `apps/docs/tdd-ab-image-ota.md`.

### 3.6 Cloud feed ingest — upload & fetch-from-URL (cloud-side)

Everything above is the **device** consuming a firmware URL. Separately, the
**cloud** operator has to get an artifact *into* the feed
(`firmware_dir`, served at `/firmware/`) + `cloud.firmware.manifest` before it
can be pushed. Two iot-httpd routes do this (both admin-only, both no-op with a
`400` on a device — `firmware_dir` is empty there):

1. **Upload** — `POST /api/v1/firmware/upload` (chunked). The cloud-ui
   drag-drops a `.ipk`/bundle straight up into the feed. Server computes the
   sha256 and upserts the manifest row. (§ handler `firmware/upload`.)
2. **Fetch-from-URL** — `POST /api/v1/firmware/fetch`
   `{url,name,version,arch,pkg,sha256?}`. The operator gives an **external**
   http(s) link (a CI artifact, a release asset, a CDN); the cloud downloads it
   **server-side** into the feed, sha256-verifies (optionally against the
   supplied pin), and upserts the manifest — so a large bundle never has to
   transit the operator's browser. The download runs in a **detached thread**
   (curl, TLS-verified — the cloud runtime image ships curl + ca-certificates)
   and the request returns `202` immediately; progress/completion is published
   on **`cloud.firmware.fetch.status`** (`{state:downloading|verifying|done|
   error, …}`), which the cloud-ui **Software Update** page observes.

Both paths land the artifact in the **same** feed + manifest, after which the
existing push flow (`cloud.update.request` → iot-cloudd validate → lwm2m-dm
Object-5 WRITE `/5/0/1` + EXECUTE `/5/0/2` → device `iot-ota-stage`) is
**identical** — the device always pulls the firmware from the cloud feed URL
and sha-verifies it, exactly as before. Fetch-from-URL is purely a **cloud feed
ingest** convenience; it adds **no device-side change** and never hands the
device the external URL.

**Security note.** The fetch URL is admin-supplied but still validated
(`is_safe_fetch_url`: scheme `http(s)://` only, no whitespace/control-char/quote
so it is safe to single-quote into the curl argv). TLS is verified on the
external fetch (unlike the device stager's sha-gated `-k`). The optional
`sha256` pin lets the operator fail-closed on an unexpected artifact.

## 4. Reboot-vs-restart decision (requirement 5 — decided here)

**Policy = `auto` by default.** After a successful `opkg install`:

1. **Explicit override** (from `update.meta` `reboot=`):
   `always` → reboot; `never` → restart only.
2. **`auto`** (default): reboot **iff** the install touched a base/kernel/boot
   component — any installed package matching
   `kernel*`, `linux-*`, `*-image-*`, `u-boot*`, `*-bootloader`, `base-files`,
   `systemd` — **or** an opkg postinst created `/run/reboot-required` (the
   Debian/`update-notifier` convention, honored if a package opts in).
   - **Reboot path:** results already persisted to ds (step 6) → `logger` →
     `systemctl reboot`. tmpfs spool is wiped by the reboot.
   - **Restart path** (iot userspace `.ipk` — the common case): no reboot;
     `systemctl try-restart` the affected units
     (`iot-lwm2m-client`, `iot-httpd`, `iot-openvpn-client`, `iot-net-router`,
     `iot-wifi-client`, and `iot-ds.service` last if its package changed) so the
     device runs the new packages immediately (requirement 5, "else start using
     new iot packages").

Rationale: iot daemons are relocatable userspace — a restart is faster and
keeps the tunnel/registration churn minimal. Only kernel/bootloader/base
changes genuinely require a reboot, and we detect those from what opkg actually
installed rather than trusting the cloud. The cloud can still force either way
via `reboot=` when it knows better (e.g. a coordinated fleet reboot window).

## 5. Data-store keys — **reuse, no new keys**

The existing OTA keys (`modules/data-store/schemas/iot.lua:148`) carry the whole
state machine unchanged; responsibilities just split across the two scripts:

| Key | Stager | Installer | Meaning |
|-----|--------|-----------|---------|
| `iot.update.request` | (watched by lwm2m client → launches stager) | — | `.ipk` URL (+`?sha256&version&reboot`) |
| `iot.update.state` | 1 downloading → 2 downloaded | 3 updating → 0 idle | progress |
| `iot.update.result` | 5 on sha mismatch / 8 on download fail | 1 success / 9 install fail | outcome |
| `iot.update.version` | — | installed version | post-apply version |

device-ui + cloud-ui software-update pages already long-poll these — **no UI
change**. Cloud push (Object 5 WRITE `/5/0/1` + EXECUTE `/5/0/2`) is unchanged;
only what the device *does* on apply changes.

(No new ds key for reboot policy — it rides the existing URL query param into
`update.meta`, honoring "don't add ds keys unnecessarily".)

## 6. Security / privilege boundary

- Spool is **root-only**; the unprivileged lwm2m client (`engineer`) never
  writes it — it triggers the root stager via the existing `systemd-run` path.
- `iot-swupdate.service` runs as root (required for `opkg` + `systemctl` +
  `reboot`). It has **no `[Install]`** and is reachable only via the `.path`
  unit — it cannot be started by an unprivileged ds write.
- The trigger is **empty**; metadata is a separate root-written sidecar, so a
  hypothetical non-root writer to the spool still can't smuggle install metadata
  (and can't write the spool at all at 0755 root).
- sha256 is verified by the stager **before** the trigger is written, so the
  installer only ever sees vetted packages.

## 7. Failure modes & recovery

| Failure | Handling |
|---------|----------|
| Download fails / empty | stager: `result=8`, state 0, **no trigger** → installer never runs |
| sha256 mismatch | stager: `result=5`, state 0, **no trigger** |
| `opkg install` fails | installer: `result=9`, state 0, keep `last.log`, **no reboot/restart**; trigger already removed (no loop) |
| Crash mid-install | flock released on exit; tmpfs spool wiped next reboot; `.path` re-fires only if a trigger is re-staged |
| Concurrent campaigns | flock serializes; 2nd activation queued by systemd after the 1st `.service` exits |
| Half-written `.ipk` raced in | installer moved its set into `.work-<pid>` before installing; new file waits for the next trigger |
| Reboot mid-restart-set | only taken when auto/override says reboot; results persisted to ds first |

## 8. Yocto recipe changes (`yocto/meta-iot/recipes-iot/lwm2m/`)

New `files/`:
- `iot-ota-stage` (refactor of `iot-ota-apply`: download/verify/stage/touch)
- `iot-swupdate` (installer: opkg + reboot/restart decision)
- `iot-swupdate.path`, `iot-swupdate.service`

`iot_git.bb`:
- `SRC_URI += file://iot-ota-stage file://iot-swupdate file://iot-swupdate.path
  file://iot-swupdate.service`
- `do_install`: `install -m 0755` the two scripts to `${bindir}`; install the
  two units to `${systemd_system_unitdir}`; add
  `d /run/iot/update 0755 root root -` to the shipped tmpfiles.d.
- `FILES:${PN}-lwm2m += ${bindir}/iot-ota-stage ${bindir}/iot-swupdate
  ${systemd_system_unitdir}/iot-swupdate.path
  ${systemd_system_unitdir}/iot-swupdate.service`
- `SYSTEMD_SERVICE:${PN}-lwm2m += iot-swupdate.path` (the `.service` is
  activated by the `.path`, not enabled directly — same as `iot-vpn-cert`).
- Decide: keep `iot-ota-apply` for one release as a compat shim (calls
  iot-ota-stage) or remove it and bump `ota_launch_apply` in the same PR.
  **Recommendation: remove** `iot-ota-apply`, switch `ota_launch_apply` →
  `iot-ota-stage` in the same change (single source of truth; nothing external
  calls the old name).

C++ (`apps/src/main.cpp`): one-line change in `ota_launch_apply` — run
`iot-ota-stage` instead of `iot-ota-apply` (and rename the transient unit). No
logic change; the Object-5 / `iot.update.request` wiring is untouched.

## 9. Test plan

- **Unit (host):** none required for shell; keep the C++ `ota_launch_apply` arg
  build under the existing apps test compile.
- **QEMU/RPi manual:**
  1. Stage a userspace `.ipk` (`iot-httpd`) → assert: trigger consumed, opkg
     installed, `iot-httpd` restarted, **no reboot**, `iot.update.result=1`,
     `iot.update.version` set.
  2. Stage a `base-files`/`kernel` `.ipk` (or `update.meta reboot=always`) →
     assert reboot taken, results persisted pre-reboot, post-boot ds reflects
     them, spool empty.
  3. sha256 mismatch → `result=5`, no trigger, no install.
  4. opkg failure (corrupt `.ipk`) → `result=9`, no restart, `last.log` present.
  5. Re-arm: stage twice back-to-back → both install, no loop, trigger absent at
     rest.
- **Cloud e2e:** existing cloud-ui multi-select push → device applies via the
  new path; cloud `cloud.update.status` reflects success (unchanged plumbing).

## 10. Decisions

1. **Watcher = systemd `.path` unit** ✅ (confirmed 2026-06-14) — reuse the
   `iot-vpn-cert.path` inotify pattern; no custom daemon.
2. **Reboot policy = `auto` + cloud override** ✅ (confirmed 2026-06-14) — reboot
   only on a base/kernel/boot package (glob list in §4) or `/run/reboot-required`;
   else restart the iot daemons. Cloud forces via `?reboot=always|never`.
3. **Drop `iot-ota-apply`** in the same PR (recommended) — switch
   `ota_launch_apply` → `iot-ota-stage`; nothing external references the old
   name. *(Pending final nod; recommended.)*
4. **`update.meta` = plain `key=value`** — shell-friendly, transient `/run`
   spool sidecar (not persistent config, so outside Project-Rule-2 Lua).
   *(Pending final nod; recommended.)*
5. **Migration marker = ds key `iot.config.version`** (integer) ✅ — chosen over
   reusing the persistor's `schema_version` (which versions the on-disk *format*,
   not the app config generation). See §11.

## 11. Config & schema migration

The data store separates two things, and an `opkg upgrade` treats them
differently — this is what makes most upgrades safe with no work:

| | Path | Shipped by `.ipk`? | On upgrade |
|---|---|---|---|
| **Schema** (key type/default/ACL) | `/etc/iot/ds-schemas/*.lua` | yes | **overwritten** — MUST stay non-conffile (see below) |
| **Persisted values** (what was `set`) | `/var/lib/iot/data_store.lua` | **no** (ds-server writes it at runtime) | **preserved** (opkg never touches `/var/lib`) |

ds-server boots, loads the **new** schema (`SchemaRegistry::load_directory`),
then blind-bulk-loads the **old** values (`DataStore::load_from` — no
validate/prune). So:

- **Key added** (new feature) → **automatic, no migration.** `data_store.lua`
  has no value → `get` falls through to the schema `default` (`default_for`); the
  daemon seeds/uses it.
- **Key deleted** → the old value lingers as a harmless **orphan** in
  `data_store.lua` (nothing prunes it; `get` returns it as unknown-key
  passthrough, `set` may be rejected if the namespace is still claimed). Optional
  prune via a migration.
- **Key renamed / retyped / semantically changed** → **needs an explicit
  migration.** `load_from` doesn't coerce, so a retyped key keeps its old-typed
  value, and a renamed key orphans the old value while the new one gets its
  default (value not carried). These are the only cases that require work.

**MUST:** the schema files stay **plain files, not opkg conffiles** (confirmed:
no `CONFFILES` in the recipe). If they were ever marked conffiles, an upgrade
would keep the *old* schema and new keys would silently never appear.

### 11.1 Migration runner (in `iot-swupdate`, after opkg, before app restart)

- **Marker:** ds key `iot.config.version` (integer, default 0) — the applied
  config generation; persisted, survives reboot.
- **Migrations:** ordered, idempotent scripts shipped in the `.ipk` at
  `/usr/share/iot/migrations/NNNN-<slug>.sh` (zero-padded number = the config
  generation it produces). Each does its rename/retype/prune/seed via `ds-cli`.
- **Sequence** (§3.4 step 7):
  1. `systemctl restart iot-ds.service`; wait until it's healthy (so it has the
     **new** schema loaded — migrations' `ds-cli` writes validate against it).
  2. `cur = ds-cli get iot.config.version` (default 0).
  3. For each `NNNN-*.sh` with `NNNN > cur`, ascending: run it; on success
     `ds-cli set iot.config.version NNNN`. (So after the last one,
     `iot.config.version` = the highest migration shipped — no separate "target".)
  4. On any migration failure: stop, leave `iot.config.version` at the last good
     value, `iot.update.result = 9`, log, and **do not** restart the app daemons
     (don't run them against a half-migrated store) — exit non-zero for operator
     intervention.
- **Idempotency:** the version guard runs each migration once; scripts are also
  written defensively (e.g. "set new only if old exists; then delete old").
- **Adds-with-default need NO migration entry** — only renames/retypes/prunes do.
- **Forward-only:** a downgrade does not auto-reverse migrations (newer orphans
  linger; the older schema serves defaults). Documented limitation.

### 11.2 Example migration (`0001-rename-foo-to-bar.sh`)

```sh
#!/bin/sh
# Carry iot.old.key → iot.new.key, then prune the old one. Idempotent.
S=--socket=/run/iot/data_store.sock
old=$(ds-cli $S get iot.old.key 2>/dev/null | sed 's/^iot.old.key=//')
[ -n "$old" ] && ds-cli $S set iot.new.key "$old"
ds-cli $S del iot.old.key 2>/dev/null || true
```

### 11.3 What this adds to the implementation

- ds schema: new key `iot.config.version` (`iot.lua`).
- `iot-swupdate`: the §11.1 runner (restart ds → loop migrations → bump marker).
- recipe: ship `/usr/share/iot/migrations/` (FILES + install); seed one example.
- No change to the stager, the `.path`, or the cloud/UI side.
