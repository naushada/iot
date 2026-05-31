# L11 Plan — Packaging + Deployment

> Forward-looking phase plan. Mirrors L10/results.md shape but
> pre-execution: D-items listed with risk gates + tests to write +
> on-disk artefacts. As each D closes, the corresponding section
> moves into `results.md` alongside the evidence.
>
> **Status (2026-05-31):** D1 + D2 done (PR #23). D3–D6 pending.

---

## 0. Goal

Make `ds-server` + `lwm2m` deployable as real services on a target
device. Today everything is dev-built: binaries live under
`apps/build/` and `modules/data-store/build/`, no init system
integration, no canonical install layout, no runtime container image.
After L11, an operator can:

1. Build a single OCI image (or install a `.tar.gz`).
2. `systemctl enable iot-ds.service iot-lwm2m.service`.
3. Tune `iot.*` keys via `ds-cli` against the live `ds-server`.

without copying binaries by hand, picking socket paths, or knowing
that `liblua5.3-dev` needs to be on the runtime host.

### Non-goals

- **Debian/RPM packaging.** OCI + tarball are enough for v1. `.deb`
  is a follow-up if a packaging team asks.
- **Multi-arch images.** Build for the dev arch (arm64 on the M2) +
  amd64 if cross-builds are free; don't invest in QEMU plumbing.
- **systemd socket activation.** Static units are enough; `Type=simple`
  with `After=` ordering covers the dependency between ds-server and
  the lwm2m client/server.
- **Hot config reload via sighup.** Already covered by the FUP-DS-7
  through FUP-DS-11 work — operators drive config via `ds-cli`, not
  by restarting units.
- **TLS / mTLS to ds-server.** The unix socket + DAC perms are the
  authentication boundary in v1.

---

## 1. Risk register

| ID    | Risk                                                                                            | Mitigation                                                              |
|-------|--------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------|
| R1    | ACE_TAO + tinydtls + Lua statics must be present in the runtime image, not just the build image | Multi-stage Dockerfile: stage 2 copies only the `.so` ACE libs + the iot/ds-server binaries; lua is statically linked already |
| R2    | systemd unit must wait for `/var/run/iot/` to exist + be writable before ds-server can bind     | Ship a `tmpfiles.d/iot.conf` entry + `RuntimeDirectory=iot` in the unit |
| R3    | ds-server runs as a non-root `iot` user but needs write to `/var/lib/iot/data_store.lua`       | `StateDirectory=iot` in the unit + matching `mkdir`/`chown` in install rule |
| R4    | lwm2m client must come up *after* ds-server so DsConfig connect succeeds without falling back  | `After=iot-ds.service` + `Wants=iot-ds.service` in the lwm2m unit       |
| R5    | OCI image bloat — naive copy pulls hundreds of MB of dev tools                                  | Strict multi-stage; runtime image ≤ 100 MB target                       |
| R6    | Dev-machine ACE install at `/usr/local/ACE_TAO-7.0.0/lib` won't exist on a packaged host       | Bundle the ACE shared libs in the OCI image; for tarball, document `apt-get install libace-dev` as a prereq |
| R7    | The `/etc/iot/` layout shipped by cmake may clash with operator overrides                       | Install schema/config as `*.lua.example`; documented copy-then-edit     |

---

## 2. D-items

### D1 — systemd units ✅ (PR #23)

Closed 2026-05-31. cmake install rule lands in D4.

**Scope.** Author and install three unit files:

- `iot-ds.service` — runs `ds-server`. `Type=simple`, `Restart=on-failure`.
- `iot-lwm2m-client.service` — runs `lwm2m role=client …` with config sourced from `EnvironmentFile=/etc/iot/lwm2m-client.env`. `After=iot-ds.service`, `Wants=iot-ds.service`.
- `iot-lwm2m-server.service` — same shape for `role=server`. Mutually exclusive with the client unit in practice but both ship; operators enable one.

All three units run as user `iot:iot`, `RuntimeDirectory=iot`,
`StateDirectory=iot`, `LogsDirectory=iot`. `RuntimeDirectoryMode=0755`
so peer clients can `connect()` to the socket inside.

**Risk gates closed.** R2, R3, R4.

**Tests.** A `log/L11/units-smoke.sh` that, given a podman container
with systemd inside (or a sysvinit-skipping `systemd-nspawn`), runs:

1. `systemctl enable iot-ds.service`
2. `systemctl start iot-ds.service`
3. `systemctl is-active iot-ds.service` → `active`
4. `ds-cli get iot.endpoint` succeeds (returns schema default)
5. `systemctl stop iot-ds.service`; `systemctl status iot-ds.service` shows clean exit.

**Artefacts.**

- `packaging/systemd/iot-ds.service`
- `packaging/systemd/iot-lwm2m-client.service`
- `packaging/systemd/iot-lwm2m-server.service`
- `packaging/systemd/iot.conf` (tmpfiles.d entry — fallback for hosts that don't honour `RuntimeDirectory=`)
- cmake install rule

---

### D2 — Default config layout ✅ (PR #23, partial)

Closed 2026-05-31 for the parts that ship now: env files + tmpfiles.d
fallback. The `/etc/iot/config/*.lua.example` rename moves to D4 where
the install rules live.

**Scope.** Decide + ship the on-disk tree:

```
/etc/iot/
├── ds-schemas/
│   └── iot.lua                  (already installed by FUP-DS-6)
├── lwm2m-client.env             (EnvironmentFile for the client unit)
├── lwm2m-server.env             (EnvironmentFile for the server unit)
└── config/
    ├── deviceObject/0.lua.example
    ├── securityObject/0.lua.example
    ├── securityObject/1.lua.example
    └── serverObject/0.lua.example

/var/lib/iot/
└── data_store.lua               (created by ds-server on first write)

/var/run/iot/
└── data_store.sock              (created by ds-server at startup)
```

`.lua.example` files are checked-in templates; operators copy + edit
for their deployment. `apps/config/**/*.lua` becomes `.lua.example`
under `/etc/iot/config/`.

**Risk gates closed.** R7.

**Tests.** A short doc-test: `find /etc/iot /var/lib/iot /var/run/iot
-type d` after install matches the expected tree.

**Artefacts.**

- `packaging/etc-iot/lwm2m-client.env`
- `packaging/etc-iot/lwm2m-server.env`
- cmake `install(DIRECTORY apps/config/ DESTINATION /etc/iot/config/ FILES_MATCHING PATTERN "*.lua")` (renaming on install isn't natively supported by cmake; D2 may shell out to a custom command)

---

### D3 — OCI image build

**Scope.** A `packaging/Containerfile` (podman-first; works under
Docker too) producing a runtime image:

- **Stage 1 (build):** based on `naushada/iot:latest` (the existing dev
  image). Clones the repo, runs `cmake + make`.
- **Stage 2 (runtime):** based on `ubuntu:22.04` minimal. Copies:
  - `/usr/local/bin/{ds-server, ds-cli, lwm2m}`
  - `/usr/local/ACE_TAO-7.0.0/lib/*.so.*`
  - `/etc/iot/ds-schemas/iot.lua`
  - `/etc/ld.so.conf.d/ace-tao.conf` + run `ldconfig` in a layer
- ENTRYPOINT is configurable via `IOT_ROLE=ds|client|server` env var, dispatched by a small `/usr/local/bin/iot-entrypoint` shell script.

Target size ≤ 100 MB.

**Risk gates closed.** R1, R5, R6.

**Tests.** `log/L11/oci-smoke.sh`:

1. `podman build -t iot:l11 -f packaging/Containerfile .`
2. `podman run -d --name ds iot:l11 -e IOT_ROLE=ds`
3. `podman exec ds ds-cli get iot.endpoint` returns the schema default.
4. `podman run -d --name lwm2m iot:l11 -e IOT_ROLE=client …` boots without DsConfig connect errors.
5. `podman image inspect iot:l11 --format '{{.Size}}'` ≤ 100 MB.

**Artefacts.**

- `packaging/Containerfile`
- `packaging/iot-entrypoint.sh`
- `packaging/README.md` (per-stage rationale + size budget evidence)

---

### D4 — cmake install rules pass smoke

**Scope.** Audit current `install(...)` rules across
`apps/CMakeLists.txt` + `modules/data-store/CMakeLists.txt`. Add what's
missing for D1 + D2 to land:

- `install(FILES packaging/systemd/*.service DESTINATION /lib/systemd/system/)`
- `install(FILES packaging/systemd/iot.conf DESTINATION /usr/lib/tmpfiles.d/)`
- `install(FILES packaging/etc-iot/*.env DESTINATION /etc/iot/)`
- `install(DIRECTORY apps/config/ DESTINATION /etc/iot/config/ ...)` with `.example` suffix.

Verify via `make install DESTDIR=/tmp/staging` produces the expected
tree and no missing files / dangling rpaths.

**Risk gates closed.** R6 (verification path).

**Tests.** `log/L11/staging-smoke.sh`:

1. `cmake -B /tmp/build -DCMAKE_INSTALL_PREFIX=/`
2. `make -C /tmp/build install DESTDIR=/tmp/staging`
3. Assert specific paths exist: `/tmp/staging/usr/local/bin/ds-server`, `/tmp/staging/etc/iot/ds-schemas/iot.lua`, `/tmp/staging/lib/systemd/system/iot-ds.service`.
4. `ldd /tmp/staging/usr/local/bin/lwm2m` shows no `not found`.

---

### D5 — README: deploy walkthrough

**Scope.** Top-level `DEPLOY.md` (or a section in `README.md`) with
two paths:

- **Bare-metal / VM:** clone → `cmake + make + sudo make install` → `systemctl enable --now iot-ds iot-lwm2m-client`.
- **Container:** `podman pull naushada/iot-runtime:l11` → `podman run -e IOT_ROLE=ds` + `podman run -e IOT_ROLE=client`.

Both paths include a `ds-cli` configuration cookbook lifted from
`modules/data-store/docs/protocol.md` §9 (set lifetime, set endpoint,
verify get round-trips).

**Risk gates closed.** Operator-facing usability (no risk-register
item; visible quality bar).

**Artefacts.**

- `DEPLOY.md`
- Cross-links from `README.md`, `modules/data-store/docs/client_api.md`
- Cross-links from `apps/docs/architecture.md` (whatever its post-update state ends up — separately tracked).

---

### D6 — End-to-end smoke

**Scope.** A single script that runs the full deploy path on a clean
container and proves it works:

1. `podman build` the runtime image (D3 artefact).
2. `podman run` the ds + client containers with a shared volume for `/var/run/iot/`.
3. `podman exec ds ds-cli set iot.lifetime 600`.
4. `podman exec lwm2m grep -E "lifetime from data-store" /var/log/iot.log` (or whatever logging path).
5. Assert the lwm2m container picked up the override.

**Risk gates closed.** Integration-level proof that D1–D5 fit together.

**Artefacts.**

- `log/L11/e2e-smoke.sh`
- `log/L11/e2e-smoke.txt` (captured run)

---

## 3. Open questions (track to closure as D-items land)

| Q   | Question                                                                                          | Decision path                                                                       |
|-----|----------------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------|
| Q1  | Single container for both binaries (supervisord-style) vs one container per binary?               | Default: **one per binary**. Matches the systemd unit model + lets operators scale them independently. |
| Q2  | What user/group owns the install? `iot:iot`? `root:root`?                                          | **`iot:iot`** for both runtime files (sockets, data store) and the binaries. Install rule may need to `chown` post-install. |
| Q3  | EnvironmentFile vs hard-coded `ExecStart=lwm2m role=client local=...` flags?                       | **EnvironmentFile.** Operators edit `/etc/iot/lwm2m-client.env`, not the unit file. |
| Q4  | Does the OCI runtime image bundle `ds-cli` for in-container debugging?                            | **Yes.** ~50 KB; pays for itself the first time someone shells in to a misbehaving deploy. |
| Q5  | Where do logs go? journald (via systemd), file in `/var/log/iot/`, or stdout?                     | **Stdout** in container path (journald reads it); **journald** in bare-metal (systemd unit's default). ACE logging already writes to stderr so this is automatic. |

---

## 4. Sequencing

Suggested execution order (each PR independent):

1. **D1 + D2** together — units + config layout. Smallest concrete payoff (`systemctl start` works).
2. **D4** — install rules pass `DESTDIR` smoke. Unblocks D3 + D6.
3. **D3** — OCI image build.
4. **D6** — end-to-end smoke that ties D1–D5 together.
5. **D5** — README. Last because it documents the final shape, not a target moving during D1–D6.

Estimated PRs: 5–6 (D1+D2 bundled; D5 may split if it ends up large).

---

## 5. Related docs

- [L10 results](../L10/results.md) — preceding phase (data-store + iot
  integration). FUP-DS-1 (fsync profiling, opportunistic) and
  FUP-DS-12 (CoAPs DTLS rebind, conditional on deployment) remain.
- [data-store/docs/protocol.md](../../modules/data-store/docs/protocol.md)
  — wire spec, referenced by D5 walkthrough.
- [data-store/docs/client_api.md](../../modules/data-store/docs/client_api.md)
  — app integration, referenced by D5.
- [apps/docs/architecture.md](../../apps/docs/architecture.md) — likely
  stale; out-of-scope for L11 but should be refreshed before D5 lands
  so the deploy doc cross-links to something accurate.
