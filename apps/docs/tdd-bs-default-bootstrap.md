# Design Analysis — Baked-in Default Bootstrap Credentials (Zero-touch BS)

Status: **REJECTED — analysis only, NOT being implemented.**

This document captures a proposed zero-touch bootstrap scheme (shared
manufacturer-default BS PSK baked into the Yocto image, used to mint a
per-device BS credential on first contact) and the reasons it is **not** being
built. It exists so the idea — and why it was passed over — is on the record
and we don't relitigate it from scratch.

Related: `apps/docs/tdd-psk-provisioning.md` (the per-device PSK design that is
actually shipped), `apps/docs/tdd-wifi-credentials-seed.md` (the image-bake
pattern this proposal mirrored), `DEPLOY.md` § "Point the device at your cloud".

## 1. The proposed flow

A two-stage / "factory" bootstrap for zero-touch devices:

- **Stage 0:** the image ships a shared, well-known `bs.default.id` +
  `bs.default.psk`. If the per-device `iot.bs.psk.*` keys are empty, the device
  opens the BS DTLS session with the **default** credential. The BS server
  holds the same default and uses the presented identity to pick the PSK and
  decrypt.
- **Stage 1:** the BS reads the endpoint from the `/bs` request (`ep=` in the
  CoAP URI), mints a **per-device** `{id, psk}`, writes it into the device as
  its new BS credential, the device **tears down the default BS DTLS session
  and reconnects** with the per-device credential, and from there the existing
  bootstrap → DM flow runs unchanged.

In short: use a shared manufacturer PSK to bootstrap a unique bootstrap
credential.

## 2. How it would have been built (mapped to current code)

1. **Bake the default into the schema**, mirroring the WiFi seed: a
   `gen_bs_default.py` (clone of
   `yocto/meta-iot/recipes-iot/lwm2m/files/gen_wifi_default.py`) + a gitignored
   `bs_credentials.lua` (+ `.sample`), wired into `SRC_URI` only when present
   (`iot_git.bb`). New schema keys in `modules/data-store/schemas/iot.lua`:
   `bs.default.id` (string) and `bs.default.psk` (opaque,
   `read_acl/write_acl = gid:engineer`). The build rewrites their schema
   *defaults* into the baked `iot.lua`.
2. **Device fallback** (`apps/src/main.cpp`): the provisioning wait loop
   (`main.cpp:1328-1350`) parks until `iot.bs.uri` + a BS PSK exist. Extend
   `have_psk` so that when `iot.bs.psk.key` is empty it falls back to
   `bs.default.psk`.
3. **BS server mints + pushes per-device creds**: the `provisioning_resolver`
   (`main.cpp:204-310`) already builds a `Security/0` BS account with
   `identity = sha256(ep)[:32]` + `secretKey = bsKey`. For zero-touch it would
   **mint** `bsKey` on the fly (`generate_psk_hex()`) and `upsert_credential`
   into `cloud.endpoint.credentials`. The server PSK resolver
   (`main.cpp:1450-1479`) would additionally accept the default identity →
   default PSK so stage-0 handshakes succeed.
4. **Tear down + reconnect** = the existing restart path, not in-process DTLS
   rekey. The client self-exits on `iot.bs.psk.key` change
   (`should_restart_on_psk_change`, watch at `main.cpp:1734`); systemd
   `Restart=always` relaunches it with the per-device cred now non-empty.
   (In-flight CoAPs rebind does not re-handshake — FUP-DS-12.)
5. **DEPLOY.md**: a "Bake bootstrap credentials into the image" subsection
   parallel to the WiFi one.

## 3. Why we are not doing it — gaps & flaws

**🔴 1 — Shared default PSK is a fleet-wide class secret with no forward
secrecy.** tinydtls here uses `TLS_PSK_WITH_AES_128_CCM_8` (PSK, **no PFS**).
Extract `bs.default.psk` from one device — it is plaintext in `data_store.lua`
and in the baked `/etc/iot/ds-schemas/iot.lua` — and you can (a) impersonate
any un-provisioned device, and (b) **passively decrypt every device's stage-0
session**. The per-device credential is delivered *inside* that stage-0
session, so the unique credential is exposed to anyone holding the shared key.
This largely defeats the purpose of minting a unique credential.

**🔴 2 — No device authentication in stage 0 → endpoint spoofing + credential
harvesting.** With shared identity+PSK the BS cannot tell which device
connected; it trusts the self-asserted `ep=` and mints/persists a credential
for whatever endpoint is claimed. An attacker with the default PSK can mint
valid per-device BS creds for arbitrary endpoints, pollute
`cloud.endpoint.credentials`, or **hijack an already-provisioned endpoint** by
re-running stage 0. Would require an endpoint allow-list / pre-registration and
a refuse-if-already-provisioned rule — i.e. the access control zero-touch was
meant to avoid.

**🟠 3 — Collides with the current "identity is derived, not stored" design.**
Today nothing stores a BS identity; both ends compute `sha256(endpoint)[:32]`
and only the *secret* is per-device. The client at `main.cpp:1385`
unconditionally derives the identity and ignores any stored one, so
`bs.default.id` would never reach the wire without a special case. Unresolved
decision: does stage 0 put `bs.default.id` on the wire (all devices identical)
or keep `sha256(endpoint)`? If the latter, `bs.default.id` is dead and only the
PSK is shared.

**🟠 4 — The extra reconnect hop is unnecessary.** Stage-1 bootstrap already
pushes the BS account **and** the DM Security/Server objects in one `/bs` cycle
(`main.cpp:296-310`). DM creds can be delivered in the same stage-0 response
that mints the per-device BS cred — the device keeps the BS cred for next boot
and proceeds straight to DM now. "Tear down and reconnect to the new BS" adds a
round trip, a restart, and failure surface for no security gain.

**🟠 5 — Restart / re-bootstrap loop hazards.** Writing the per-device key in
stage 0 triggers `should_restart_on_psk_change`. It must fire exactly once
(compare against the just-written value — the `started_bs_psk` guard at
`main.cpp:1734`) and the empty→default fallback must not count as a change.
Interplay with the `should_rebootstrap` 30s cooldown (`main.cpp:1231`) risks a
crash-loop that resets the log buffer and blocks commissioning (the failure the
parking loop was added to avoid).

**🟡 6 — Mint idempotency / retry race.** Stage 0 must be mint-once-then-reuse.
A `/bs` retry after packet loss must not re-mint, or the device that stored
attempt-1's key cannot authenticate against attempt-2's. The generate step must
be gated on "no existing record," not run unconditionally
(`upsert_credential` is idempotent but the mint is not).

**🟡 7 — Rotation & revocation are all-or-nothing.** A leaked default PSK
forces fleet-wide rotation (new schema default → re-flash/push + BS server's
accepted-default set). No per-device revocation for the default tier.

**🟡 8 — At-rest exposure is worse than WiFi.** The WiFi seed accepts
plaintext-on-device as fine for a shared default AP. A fleet-wide bootstrap PSK
has the same plaintext posture but is a far higher-value target; the WiFi risk
acceptance does not carry over.

## 4. Conclusion

A shared baked-in PSK buys zero-touch convenience at the cost of "any leaked
image credential compromises the stage-0 channel for the whole fleet, and the
per-device credential is delivered under that compromised channel." For threat
models that include untrusted networks or physical extraction, the correct
answer is **per-device factory credentials** (raw-PK/cert, or a unique
pre-shared key burned per unit) — which is essentially what the shipped
`tdd-psk-provisioning.md` design already provides (per-device
`iot.bs.psk.key`, commissioned via device-ui, never a shared default).

Decision: **not implementing.** The per-device provisioning path stays the
supported model.
