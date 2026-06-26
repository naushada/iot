# TDD Plan — Zero-touch Bootstrap via HKDF-derived per-device PSK

Status: **PROPOSED — design only, not yet implemented.**

This is the zero-touch / plug-and-play bootstrap design that replaces the
manual device-ui commissioning step **without** introducing a fleet-wide class
secret. It is the agreed alternative to the shared baked-in default PSK scheme
analysed and rejected in `apps/docs/tdd-bs-default-bootstrap.md` — read that
first; this doc only restates the parts of it that matter here.

Related:
- `apps/docs/tdd-bs-default-bootstrap.md` — the REJECTED shared-default scheme
  (why a single baked PSK is unsafe).
- `apps/docs/tdd-psk-provisioning.md` — the SHIPPED per-device PSK + serial
  design this builds directly on (`iot.bs.psk.*`, `cloud.endpoint.credentials`,
  the `engineer`/`cloud-svc` accounts, derived identity).
- `apps/docs/tdd-wifi-credentials-seed.md` — the image-bake pattern (mirrored
  here for the cloud master only).

---

## 1. Goal & definition of "zero-touch"

A device should go **flash → power on → LwM2M registered + VPN up** with no
device-ui step and no per-device action in the cloud UI. Concretely, remove
both manual steps that the shipped flow still requires:

1. device-ui generating a random `iot.bs.psk.key` and the operator revealing
   it; and
2. the operator pasting that key into the cloud Endpoints provision form so
   `cloud.endpoint.credentials` gains a row.

The replacement: every device boots already holding a **per-unit** BS PSK that
the cloud can **re-derive on demand** from a single master secret it alone
holds. No shared device secret; no per-device row to pre-create in the cloud.

### 1.1 The one hard constraint (why this is not "bake one file like WiFi")

DTLS-PSK here is `TLS_PSK_WITH_AES_128_CCM_8` — **symmetric**. Both ends need
the *same* secret, and the device must hold its secret **before first contact**
(it is what authenticates the very first `/bs` handshake). Therefore:

- You **cannot** bake a single shared file into the rootfs image and stay
  secure — every device flashed from that image would share one PSK, which is
  exactly the rejected class-secret design.
- The per-device secret **must be injected per unit** at flash/personalisation
  time. There is no symmetric scheme that avoids this.

What HKDF buys is **not** "skip per-unit injection." It buys:
- the cloud needs **no per-device registration DB** for BS — it derives the
  expected PSK from the presented serial instead of looking up a stored row; and
- the flashing tool needs **no call to the cloud** — it derives the same PSK
  locally from the master, so manufacturing is offline-capable.

So the WiFi-style "copy a file before the build" applies to the **cloud master
key**, baked into the **cloud image only**. The device side is a per-unit inject.

---

## 2. Architecture

```
        ┌──────────────────────── manufacturing / flashing host ───────────────┐
        │  holds MASTER (offline)                                               │
        │  iot-bs-personalize <serial>:                                         │
        │     psk = HKDF(MASTER, serial)  ──────┐  inject per-unit              │
        └───────────────────────────────────────┼──────────────────────────────┘
                                                 ▼
   device (per unit)                       iot.bs.psk.key   = psk      (persisted)
   ───────────────────                     iot.bs.psk.identity = serial
   first boot, no UI:                      iot.bs.psk.override = true  (identity verbatim)
     present identity = serial  ─────DTLS PSK handshake────►  cloud BS server
     present key      = psk                                   holds MASTER (cloud-only)
                                                              psk' = HKDF(MASTER, serial)
                                                              auth ⇔ psk == psk'
     ◄──── /bs response: BS account + minted DM creds ────
     persist DM creds, register to DM, VPN up
```

### 2.1 Why identity must be the **raw serial**, not `sha256(serial)`

The DTLS PSK resolver on the server sees only the **presented PSK identity**
during the handshake — the CoAP `/bs` request (which carries `ep=<serial>`)
arrives *after* the handshake completes. Today the identity is
`sha256(serial)[:32]` (`apps/src/main.cpp:2093`), which is one-way: the server
**cannot** recover the serial to feed HKDF.

So the HKDF tier puts the **raw serial on the wire as the PSK identity**, reusing
the existing `iot.bs.psk.override` path (override ⇒ identity used verbatim,
`apps/src/main.cpp:2081-2084`). The serial is not a secret — it is on the device
label and readable from `/proc/cpuinfo` — so cleartext on the wire is acceptable
(the `sha256` wrapper was light obfuscation, never an access control).

### 2.2 Why this is safe where the shared-default was not

Possession of `HKDF(MASTER, serial)` **is** the authentication. To impersonate
serial *X* an attacker must produce `HKDF(MASTER, X)`, which requires the master.

| Rejected shared-default flaw | This design |
| --- | --- |
| 🔴 class secret on every device → crack one, decrypt the fleet's stage-0 | each device holds only its **own** derived key; crack one → that unit only. Master never ships to a device. |
| 🔴 no device auth in stage 0 → spoof any `ep=` | to auth as serial *X* you need `HKDF(MASTER,X)`; an attacker can't mint it. No allow-list needed — the key gates access. |
| 🟠 collides with "identity is derived, not stored" | reuses the existing `override` verbatim-identity path; serial is the identity. No new identity concept. |
| 🟠 extra reconnect hop | none — DM creds are minted in the same `/bs` cycle (§4.3), device proceeds straight to DM. |

Residual risks (documented, accepted, or mitigated):
- **Master compromise = fleet compromise.** Mitigation: master lives **cloud-only**
  and **never in the clear** — stored AES-256-GCM-wrapped (`cloud.bs.master.key`)
  under a KEK delivered out-of-band (systemd `LoadCredential` / `IOT_BS_MASTER_KEK`),
  unwrapped into process memory only (§3.3). Extracting the cloud's data-store
  without the KEK yields ciphertext. Versioned (`v1` in both the HKDF `info` and the
  envelope AAD) for rotation. Upgrades to a managed KMS by swapping only the KEK
  source.
- **At-rest key on device** (`iot.bs.psk.key` plaintext in `/var/lib/iot`): same
  posture as the shipped per-device key, but now one-per-unit, so extraction
  leaks one unit. Acceptable for the device-ui-commissioning threat model already
  in production.
- **Serial enumeration:** RPi serials are guessable, but guessing a serial does
  not yield its key (needs master). DM-tier creds remain per-endpoint random.

---

## 3. The HKDF construction (MUST be byte-identical on both impls)

Two independent implementations must agree: the **flashing tool** (host) and the
**cloud BS server**. Pin every parameter:

```
PRK/OKM = HKDF-SHA256(
    ikm   = MASTER_KEY_BYTES,        # raw bytes of the master (see §3.1)
    salt  = "" (zero-length)         # RFC 5869 §2.2: defaults to HashLen zeros
    info  = "iot-bs-psk:v1:" + serial   # ASCII; serial = raw device serial
    L     = 32                       # 32-byte / 256-bit output
)
iot.bs.psk.key (stored) = lowercase_hex(OKM)   # 64 hex chars, opaque type
```

- **`info` includes a version tag (`v1`)** so the master can be rotated /
  re-keyed by bumping to `v2` without changing the serial namespace.
- **`info` binds the serial** so each unit's key is independent.
- **Output is hex-encoded** to match the existing `opaque`/hex convention for
  `iot.bs.psk.key` and `cloud.endpoint.credentials[].bs.psk.key`.

### 3.1 Master key format

- `cloud.bs.master.key` is the master, stored as **hex** (opaque), e.g. 32 bytes
  → 64 hex chars. The IKM fed to HKDF is the **decoded raw bytes** (`hex_decode`),
  not the hex string, so both impls agree.

### 3.2 Implementations

- **C++ (cloud):** add `hkdf_sha256(ikm, salt, info, L)` to
  `apps/src/psk_gen.cpp` / `apps/inc/psk_gen.hpp` using OpenSSL
  `EVP_PKEY_derive` with `EVP_PKEY_HKDF` (OpenSSL ≥ 1.1.0; already the crypto
  lib — `apps/src/psk_gen.cpp` includes `<openssl/evp.h>`). Add a convenience
  `derive_bs_psk_hex(master_hex, serial)` that does
  `hex(HKDF(hex_decode(master_hex), "", "iot-bs-psk:v1:"+serial, 32))`.
- **Host flashing tool:** `yocto/meta-iot/recipes-iot/lwm2m/files/gen_bs_psk.py`
  (sibling of `gen_wifi_default.py`), HKDF via `hmac`+`hashlib` stdlib (no
  `cryptography` dep), producing the identical 64-hex string. Ship a
  `test_gen_bs_psk.py` with RFC 5869 test vectors **and** a cross-check vector
  shared with the C++ unit test so drift is caught.

> **Status (P1, done):** `iot::hkdf_sha256` + `iot::derive_bs_psk_hex` shipped;
> `gen_bs_psk.py` + `test_gen_bs_psk.py` shipped. Cross-check vector
> `master=0001…1e1f, serial=100000003d1f9c2e → 223a82da…600d4fe5` asserted on
> both sides. Verified: Python 13/13, C++ in the `lwm2m_test` gtest suite.

### 3.3 Master at rest — AES-256-GCM envelope (decided: KMS-wrapped)

The master is **never stored in the clear**. `cloud.bs.master.key` holds:

```
base64( nonce(12) || AES-256-GCM(KEK, master)_ciphertext || tag(16) )
   AAD = "iot-bs-master:v1"     # binds the blob to purpose+version
```

- **KEK** (32-byte AES-256 key) is delivered to `iot-lwm2m-server` **out-of-band**
  — systemd `LoadCredential=bs_kek:/etc/iot/bs-master.kek` (root-only 0400) or env
  `IOT_BS_MASTER_KEK` — and is **never** written to the data-store. The server
  unwraps once at startup and holds the master in process memory only.
- No external service: this is envelope encryption local to the cloud box (fits the
  Vultr self-hosted deployment). To upgrade to a managed KMS later, swap only the
  KEK source (or replace the GCM open with a KMS `Decrypt`) — the wire/at-rest
  format and the resolver are unchanged.
- **Single implementation:** wrap+unwrap are C++/OpenSSL only (`iot::wrap_bs_master`
  / `iot::unwrap_bs_master_hex`), because Python here has no AES-GCM. The host
  `bs-master-wrap` CLI (P2b) reuses the same code, so there is no cross-language
  envelope to keep in sync — only the HKDF tool stays pure-stdlib.
- **Fail-closed:** a bad/missing KEK, wrong KEK, or a tampered blob yields
  `nullopt` → the master is treated as absent → HKDF tier disabled (the
  commissioned per-device tier still serves). Never derive against a bad master.

> **Status (P2, this PR):** `base64_encode/decode`, `wrap_bs_master`,
> `unwrap_bs_master_hex` shipped + tested (fixed-vector + round-trip + tamper +
> wrong-key + fail-closed). The `resolve_bs_psk` / `should_mint_dm` decision
> helpers (§4.3) shipped + tested. `cloud.bs.master.key` schema + `gen_bs_master.py`
> shipped. **Not yet wired:** the `main.cpp` BS-server resolver/mint call sites,
> the `bs-master-wrap` CLI, the `IOT_BS_SEED` recipe bake, and KEK delivery — P2b.

---

## 4. Component changes (mapped to current code)

### 4.1 New schema keys

**Cloud** — `modules/server/lwm2m/schemas/cloud.lua` (next to `cloud.bs.psk.*`,
`cloud.endpoint.credentials` at ~:262):

```lua
["cloud.bs.master.key"] = {           -- HKDF master, cloud-only, AES-256-GCM-
    access    = "Admin",              -- wrapped (§3.3). Stores the base64 envelope,
    type      = "opaque",             -- NOT the raw master.
    default   = "",                   -- empty ⇒ HKDF tier disabled (see §4.3)
    write_acl = {"gid:cloud-svc"},
    read_acl  = {"gid:cloud-svc"},    -- write-only to ds-cli, like the PSK keys
}
```

**Device** — no new keys. The HKDF tier **reuses** the shipped keys:
`iot.bs.psk.identity` (= serial), `iot.bs.psk.key` (= derived), `iot.bs.psk.override`
(= true). See `modules/data-store/schemas/iot.lua`.

### 4.2 Cloud master seed (WiFi-pattern, cloud image only)

Mirror the WiFi seed exactly, scoped to the cloud build:
- `bs_master.lua.sample` (tracked) + gitignored `bs_master.lua` holding
  `return { master = "<64 hex>" }`.
- `gen_bs_master.py` rewrites the `cloud.bs.master.key` `default = '…'` in the
  installed `cloud.lua` when `bs_master.lua` is present, exactly as
  `gen_wifi_default.py` rewrites `wifi.networks` (`iot_git.bb:421-431`, gated by
  an `IOT_BS_SEED == '1'` SRC_URI add mirroring `IOT_WIFI_SEED` at
  `iot_git.bb:99`).
- **Cloud build only.** The device image must NOT carry the master — assert this
  in review and (ideally) a build check that `cloud.bs.master.key` default is
  empty in the device rootfs `iot.lua`.

### 4.3 Cloud BS server: derive-or-lookup + mint DM

**Server PSK resolver** (`apps/src/main.cpp:2160-2189`, `is_bs` branch). Today it
iterates `cloud.endpoint.credentials` matching `sha256(serial)[:32] == presented`.
Change to **lookup-then-derive**:

```
if is_bs:
    1. existing behaviour: scan cloud.endpoint.credentials for a row whose
       sha256(serial)[:32] == presented  → return its bs.psk.key   (commissioned tier)
    2. NEW: if no row AND cloud.bs.master.key is non-empty AND `presented`
       is a plausible raw serial → return derive_bs_psk_hex(master, presented)
```

Both tiers coexist → existing commissioned devices keep working; new devices use
the derived path. Empty master ⇒ step 2 is skipped ⇒ behaviour is unchanged.

**Provisioning resolver** (`apps/src/main.cpp:252-389`). It receives `ep=serial`
directly from the `/bs` CoAP request, so HKDF is trivially available here.
For a serial with **no** `cloud.endpoint.credentials` row (zero-touch first
contact):
- `bs.identity` = serial verbatim (not `sha256(ep)`) when serving the derived
  tier, so the device's stored `override` identity matches what the BS expects.
  (Keep `sha256(ep)` for the legacy commissioned tier.)
- **Mint DM creds on the fly**: `generate_psk_hex()` → `dmKey`,
  `format_identity(serial)` → `dmId`, then `upsert_credential(...)` into
  `cloud.endpoint.credentials` so the DM server's resolver finds it on the next
  handshake. Minting happens **inside the HKDF-authenticated BS session**, so it
  carries none of the rejected design's "mint under a shared-key channel" risk.
  - **Mint-once idempotency (rejected-design flaw 🟡6):** gate the mint on "no
    existing row for this serial"; a `/bs` retry after packet loss must reuse the
    already-minted row, never re-mint. `upsert_credential` is idempotent on the
    record but the **generate** step must be guarded by a read-check.

### 4.4 Device: personalisation inject (the per-unit step)

The device must boot already holding `iot.bs.psk.{identity,key,override}`.
**Recommended (Option I):** flash-time personalisation.

- New host tool `iot-bs-personalize <serial>` (wraps `gen_bs_psk.py`): after the
  SD image is written, derive the PSK and drop a per-unit file onto the device's
  **data/boot partition** (e.g. `/var/lib/iot/bs-seed.json`
  `{ "serial":"…", "key":"<hex>" }`), NOT the shared rootfs.
- Extend **first-boot seed** `yocto/.../files/iot-ds-seed` (runs once, before
  `iot-httpd`, guarded by `/var/lib/iot/.seeded`): if `bs-seed.json` exists and
  `iot.bs.psk.key` is empty, `ds-cli set` `iot.serial`, `iot.bs.psk.identity`
  (=serial), `iot.bs.psk.key` (=derived), `iot.bs.psk.override=true`, then
  **shred** `bs-seed.json`. Idempotent and one-shot like the existing seed.
- On RPi the serial is also auto-read by the client (`set_serial`,
  `ds_config.cpp:237-261`); personalisation supplies the **key**, which is the
  only thing the device cannot self-derive.

This keeps the client unprivileged and **adds no crypto to the device** — it
stores an injected key, it never runs HKDF (the master is never on-device).

**Alternatives considered** (decision point — see §6):
- *Option II — LAN auto-commission:* a one-shot local tool reads the serial from
  the booted device and pushes the derived key over the device-ui/ds socket on
  the LAN. No flash-time step, but not headless.
- *Option III — secure element:* derive on-device from a key in an ATECC/TPM.
  Out of scope for current RPi3B hardware (no secure element).

### 4.5 Self-restart interplay (rejected-design flaw 🟡5)

The first-boot seed writes `iot.bs.psk.key` **before the client starts** (seed is
`Before=iot-httpd` and the client parks until the key exists,
`apps/src/main.cpp:2023-2037`). So the empty→derived transition happens
pre-init and does **not** trip `should_restart_on_psk_change`
(`apps/src/main.cpp:2467-2480`, `provisioning_policy.cpp:29-32`) — that guard
only fires on a change *after* `started_bs_psk` is captured. A genuine rekey
(master `v1→v2`, re-personalise) still self-restarts cleanly via systemd
`Restart=always`, which is the intended behaviour.

---

## 5. Test plan

**Unit (apps, podman per `tdd-psk-provisioning.md` §test cmd):**
- `hkdf_sha256` against the **RFC 5869** test vectors (cases 1–3).
- `derive_bs_psk_hex(master, serial)` cross-check vector shared verbatim with
  `test_gen_bs_psk.py` (catches C++/Python drift — the single most important
  test).
- Resolver: presented serial with master set & no row → derived key; with a row
  → the row's key (commissioned tier still wins); empty master → "" (disabled).
- Provisioning resolver mint-once: two `/bs` for the same fresh serial → one
  mint, identical DM creds (idempotency).

**Host:** `test_gen_bs_psk.py` — RFC 5869 vectors + the shared cross-check +
malformed-master / odd-hex rejection (mirror `test_gen_wifi_default.py`).

**Integration (needs BS+DM+device, deferred like the N-wire/P tasks in
`tdd-psk-provisioning.md`):**
- Personalise a serial, boot a fresh device with **no** cloud row, confirm
  flash→registered with zero UI steps; confirm a DM row was minted.
- Negative: device with a key NOT derived from the master → BS handshake fails.
- Rekey: bump `info` to `v2` + re-personalise → device self-restarts and
  re-registers; old key rejected.

---

## 6. Decisions

| Decision | Resolution |
| --- | --- |
| **Device-side inject mechanism** | ✅ **Decided: Option I — flash-time file + `iot-ds-seed`** (fully headless, no extra hardware, matches the existing one-shot seed). P3. |
| **Master at rest in cloud** | ✅ **Decided: AES-256-GCM envelope + KEK via systemd credential** (§3.3) — no external service, fits Vultr; upgrades to a managed KMS by swapping the KEK source. Implemented in P2. |
| "Plausible raw serial" check in the resolver | ✅ Permissive — step 2 derives for any non-empty `presented` once a master is set (`resolve_bs_psk`). HKDF-key possession is the real gate; a wrong guess just fails the handshake. |
| Mint DM at BS time vs. keep device-ui paste for DM | ✅ **mint-on-`/bs`** with mint-once idempotency (`should_mint_dm`), gated by `cloud.dev.mode` / master-present. Wiring in P2b. |
| Master rotation policy | `info`+AAD version tag (`v1→v2`) + **dual-derive** accept window; document in DEPLOY.md (P4). |

---

## 7. Phasing

1. ✅ **P1 — crypto + parity (done):** `hkdf_sha256` + `derive_bs_psk_hex` (C++)
   and `gen_bs_psk.py` + tests with the shared cross-check vector. No behaviour
   change. Verified in podman (`lwm2m_test`) + Python.
2. **P2 — cloud derive-or-lookup.** Split:
   - ✅ **P2a (this PR) — testable foundation, inert:** envelope crypto
     (`base64_*`, `wrap_bs_master`, `unwrap_bs_master_hex`), the resolver
     decision helpers (`resolve_bs_psk`, `should_mint_dm`), `cloud.bs.master.key`
     schema, and `gen_bs_master.py`. All unit-tested; nothing calls them yet.
   - ⬜ **P2b — wiring + deployment:** call `resolve_bs_psk` in the BS PSK
     resolver and `should_mint_dm` + mint at `/bs` in the provisioning resolver
     (`apps/src/main.cpp`); the `BsMasterProvider` startup unwrap (KEK from
     `IOT_BS_MASTER_KEK` / systemd cred); the `bs-master-wrap` CLI; the
     `IOT_BS_SEED` recipe bake of a wrapped master into the cloud image. Guarded
     by empty-master = no-op, so it stays inert until a master is seeded.
3. **P3 — device personalisation:** `iot-bs-personalize` + `iot-ds-seed`
   extension + `iot.bs.psk.override` wiring (Option I, flash-time).
4. **P4 — DEPLOY.md** "Zero-touch bootstrap (HKDF)" section + master-rotation
   runbook; integration validation on real BS+DM+device hardware.
