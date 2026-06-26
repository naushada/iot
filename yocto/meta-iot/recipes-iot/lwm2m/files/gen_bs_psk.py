#!/usr/bin/env python3
"""Host-side derivation of a device's per-unit Bootstrap (BS) PSK.

Zero-touch bootstrap (see apps/docs/tdd-bs-hkdf-zerotouch.md): the cloud holds a
single master secret and re-derives each device's BS PSK on demand from the
device serial; the manufacturing/flashing host derives the SAME key locally
(offline, no cloud call) and injects it into the unit being flashed.

This module is the host half. It MUST produce byte-for-byte the same key as the
cloud C++ `iot::derive_bs_psk_hex` (apps/src/psk_gen.cpp). The shared
cross-check vector in test_gen_bs_psk.py (mirrored in apps/test/psk_gen_test.cpp)
guards against the two implementations drifting.

Construction (pinned — do not change without bumping the `v1` tag and both
implementations):

    psk = HKDF-SHA256(ikm  = bytes.fromhex(master_hex),
                      salt = "" -> 32 zero bytes  (RFC 5869 §2.2),
                      info = b"iot-bs-psk:v1:" + serial,
                      L    = 32)
    iot.bs.psk.key = psk.hex()        # 64 lowercase hex chars

Pure stdlib (hmac + hashlib) so it runs on a bare manufacturing host with no
`cryptography` package.

Usage:
    gen_bs_psk.py <master> <serial>

`<master>` is either a 64-hex master key directly, or a path to a
`bs_master.lua` file of the form `return { master = "<hex>" }`. Prints the
64-hex per-unit BS PSK to stdout.
"""

import hashlib
import hmac
import os
import re
import sys

INFO_PREFIX = b"iot-bs-psk:v1:"   # MUST match iot::derive_bs_psk_hex
PSK_BYTES = 32


# ───────────────────────────────── HKDF ─────────────────────────────────────

def hkdf_sha256(ikm: bytes, salt: bytes, info: bytes, length: int) -> bytes:
    """HKDF-SHA256 (RFC 5869), extract-then-expand.

    A zero-length salt is substituted with HashLen zero bytes per RFC 5869
    §2.2 — matching the C++ side, which does the same substitution.
    """
    hash_len = hashlib.sha256().digest_size
    if not salt:
        salt = b"\x00" * hash_len
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()       # extract
    okm, block, counter = b"", b"", 1                        # expand
    while len(okm) < length:
        block = hmac.new(prk, block + info + bytes([counter]),
                         hashlib.sha256).digest()
        okm += block
        counter += 1
    return okm[:length]


def derive_bs_psk_hex(master_hex: str, serial: str) -> str:
    """Per-unit BS PSK as 64 lowercase hex chars. Mirrors the C++ helper.

    Raises ValueError on an empty / non-hex / odd-length master so the build or
    flashing step fails loudly rather than injecting a wrong key. (The C++ side
    returns "" to mean "tier disabled"; here we hard-fail because a host asked
    to derive a key must have a real master.)
    """
    master_hex = master_hex.strip().lower()
    if not master_hex:
        raise ValueError("master key is empty")
    if len(master_hex) % 2 != 0 or not re.fullmatch(r"[0-9a-f]+", master_hex):
        raise ValueError("master key is not valid lowercase hex")
    serial = serial.strip()
    if not serial:
        raise ValueError("serial is empty")
    ikm = bytes.fromhex(master_hex)
    info = INFO_PREFIX + serial.encode()
    return hkdf_sha256(ikm, b"", info, PSK_BYTES).hex()


# ──────────────────────────── master key loading ────────────────────────────

_MASTER_RE = re.compile(
    r"""master\s*=\s*['"]([0-9a-fA-F]+)['"]""")


def load_master(master_or_path: str) -> str:
    """Return a hex master from either a literal hex string or a bs_master.lua
    file (`return { master = "<hex>" }`)."""
    if os.path.isfile(master_or_path):
        with open(master_or_path, encoding="utf-8") as fh:
            text = fh.read()
        m = _MASTER_RE.search(text)
        if not m:
            raise ValueError("no `master = \"<hex>\"` found in %s"
                             % master_or_path)
        return m.group(1)
    return master_or_path


# ───────────────────────────────── CLI ──────────────────────────────────────

def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: gen_bs_psk.py <master|bs_master.lua> "
                         "<serial>\n")
        return 2
    try:
        master_hex = load_master(argv[1])
        sys.stdout.write(derive_bs_psk_hex(master_hex, argv[2]) + "\n")
    except (ValueError, OSError) as exc:
        sys.stderr.write("gen_bs_psk: %s\n" % exc)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
