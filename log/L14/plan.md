# L14 Plan — full-stack end-to-end smoke

> Forward-looking phase plan. Same shape as L11/L12/L13. No new
> daemons — L14 composes the four we already have (ds, lwm2m,
> openvpn-client, net-router) into a single demonstrable workflow
> that proves L11–L13 actually fit together.
>
> **Status (2026-05-31):** plan only.

---

## 0. Goal

After L13 closed, every daemon is unit-tested in isolation and we
have per-daemon packaging (systemd unit + container role). What's
missing is one repeatable check that says "yes, all four daemons
brought up together produce the workflow we designed for."

L14 ships a single `podman-compose`-style harness that:

1. Boots `ds-server`, `lwm2m-client`, `openvpn-client`, `net-router`,
   plus a real `leshan` lwm2m server (we already keep its compose
   file at `docker/docker-compose.leshan.yml`).
2. Seeds the required ds keys (`iot.endpoint`, `iot.server.uri`,
   `vpn.remote.host`, `net.lwm2m.target.ip`, …) via `ds-cli`.
3. Asserts each daemon reached its "ready" state (registered with
   leshan; openvpn-client published a `vpn.state`; net-router
   applied a non-empty nftables ruleset).
4. Tears the whole thing down on success or failure.

The smoke runs locally via `bash log/L14/smoke.sh` and is the
canonical reproducer for any bug that says "broke after I deployed
all of them together."

### Non-goals

- **CI integration.** L14 is a manual reproducer; wiring it into a
  GitHub Actions job is FUP if we ever set up CI for this repo.
- **Real OpenVPN server.** Standing up an openvpn server with cert
  material in the smoke is heavy and orthogonal — we mock the
  openvpn binary the same way L12's smoke does (`fake-openvpn.sh`)
  so the daemon reaches its `connecting` / `running` state without
  needing a tunnel peer.
- **Real `nft` apply.** Rootless podman can't write to the host's
  nftables table. We point `--nft=` at a `fake-nft.sh` recorder
  that captures every applied ruleset to a file the smoke asserts
  on. Real-kernel apply is covered by `apply_test` (D6) + the
  manual "ssh to a Linux host and `systemctl start`" path in
  DEPLOY.md.
- **Hot-reload / chaos coverage.** L14 proves the happy path. A
  separate L15 could add "kill openvpn-client mid-tunnel and prove
  net-router degrades cleanly" + similar.
- **podman-compose vs docker-compose.** We standardise on `podman
  play kube` (or a hand-written podman-compose.yml — TBD in D1)
  because podman is the user-mandated runtime.

---

## 1. Risk register

| ID  | Risk                                                                                     | Mitigation                                                                                              |
|-----|------------------------------------------------------------------------------------------|--------------------------------------------------------------------------------------------------------|
| R1  | Rootless podman can't share /run between containers via tmpfs                             | Use a named volume `iot-run` for `/run/iot/`; the same workflow DEPLOY.md already documents.            |
| R2  | leshan's published port not stable across versions                                         | Pin the leshan image tag in the compose file; record exact tag in `log/L14/README.md`.                  |
| R3  | net-router needs to see ds-server's socket at startup; race if it starts before ds         | systemd's `After=iot-ds.service` doesn't apply in compose. Use a 10s retry loop in the smoke script, mirroring the entrypoint expectation. |
| R4  | Fake openvpn / nft recorder paths leak between test runs                                   | Smoke wraps every fake in a per-run tempdir (mktemp -d); teardown removes it.                           |
| R5  | Smoke flakes if leshan registration takes longer than the polling window                   | Generous deadlines (60s for first daemon, 120s for leshan registration). Log every poll cycle.          |
| R6  | iot image not built locally; smoke fails with "image not found"                            | Smoke script first runs `podman build -f packaging/Containerfile -t localhost/iot:l14 .` if the image is absent. |
| R7  | Asserting on `nft` recorder output couples the smoke to nft_rules generator text          | Assert only on stable substrings (`flush table inet iot_router`, `dnat to <target_ip>:80`). Brittle exact-match is FUP. |

---

## 2. Decomposed work

### D1 — compose harness

`log/L14/podman-compose.l14.yml` (or `kube.yml`, decided at D1) +
`log/L14/fake-openvpn.sh` + `log/L14/fake-nft.sh`.

- One pod with five containers: `ds`, `lwm2m`, `ovpn`, `netr`,
  `leshan`. Named volume `iot-run` shared between the first four.
- `ovpn` and `netr` override their default `--openvpn=` / `--nft=`
  args to point at the fakes (each fake is COPY'd into the iot
  image via a thin Containerfile.l14 layer, or bind-mounted in).
- `leshan` exposes 5683/5684/8080 on the host so the smoke can
  curl its REST API for the registered-endpoints list.

**Decision pending at D1:** podman-compose.yml vs podman play kube
vs a `podman run` orchestration shell script. The shell script is
the simplest path (no extra tooling), but a kube manifest reads
nicer and ports trivially to a future GHA / k3s smoke.

### D2 — smoke driver

`log/L14/smoke.sh`. Synchronous orchestrator:

1. `podman build` the iot image if absent.
2. Spin up the compose (D1 artifact).
3. Wait until `ds-server` accepts connections on its unix socket
   (poll `ds-cli get iot.endpoint` until exit 0).
4. Seed every required ds key: `iot.endpoint`, `iot.server.uri`,
   `vpn.remote.host`, `net.lwm2m.target.ip`, `net.iface.eth.name`.
5. Wait for the lwm2m-client to register with leshan
   (`curl /api/clients` until our endpoint appears).
6. Assert `vpn.state` reached `connecting` (or `running` if the
   fake openvpn returns a PUSH_REPLY).
7. Assert the fake-nft recorder file contains a ruleset with our
   `net.lwm2m.target.ip` substring.
8. Tear down (`podman pod rm -f`).

Exits 0 on success, non-zero with a diagnostic on the first failed
assertion. No retry-loop magic — flakes mean we missed a wait.

### D3 — docs

`log/L14/README.md`. Covers:
- One-paragraph "what this proves"
- `bash log/L14/smoke.sh` invocation
- How to inspect each daemon's logs when the smoke fails
  (`podman logs iot-ds`, etc.)
- The exact leshan image tag pinned at D1
- Pointer back to the per-module smoke scripts (L12 openvpn,
  L13 module tests) so failure-mode debugging has a starting
  point per daemon

DEPLOY.md gets a one-line link to `log/L14/README.md` so operators
can find the e2e reproducer.

---

## 3. Acceptance for L14

L14 closes when:

- `bash log/L14/smoke.sh` exits 0 on a clean machine with podman
  installed and the repo checked out (no other prerequisites).
- README documents how to run it + how to read failure output.
- Plan + 3 D-PRs (D1, D2, D3) merged.

No deferred items inherited from L13 carry over.

---

## 4. After L14

Candidate L15 phases (not committed):

- **L15a — chaos coverage.** Kill each daemon mid-flight, prove the
  others degrade cleanly. Builds on L14's compose.
- **L15b — Object 4 (Connectivity Monitoring).** Wire net-router's
  active-iface + iface_monitor into the lwm2m stack so a server can
  read the device's current bearer.
- **L15c — observability/recovery for net-router.** Watchdog,
  metrics, drift detection.

Pick one when L14 is in.
