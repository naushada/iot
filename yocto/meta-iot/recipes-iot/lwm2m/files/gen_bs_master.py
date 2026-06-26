#!/usr/bin/env python3
"""Build-time generator: bake a wrapped BS HKDF master into the cloud schema.

Zero-touch bootstrap (apps/docs/tdd-bs-hkdf-zerotouch.md). Mirrors
gen_wifi_default.py, but for the CLOUD image only: when an (optional,
gitignored) bs_master.lua is present, rewrite the `cloud.bs.master.key`
`default = "…"` of the already-installed schemas/cloud.lua in place with the
AES-256-GCM-wrapped master.

Pure schema rewrite — NO crypto here. The master is wrapped out-of-band by the
`bs-master-wrap` tool (C++/OpenSSL), and bs_master.lua carries only the already
-wrapped base64 envelope:

    return { wrapped = "<base64( nonce || ciphertext || tag )>" }

The wrapping KEK is NEVER baked into the image — it is delivered to the cloud
BS server at runtime (systemd LoadCredential / env IOT_BS_MASTER_KEK). So the
image holds only ciphertext; without the KEK it is inert.

Usage:  gen_bs_master.py <bs_master.lua> <installed cloud.lua>
"""

import re
import sys

# The wrapped blob is standard base64 (A–Z a–z 0–9 + / =). No quotes, so a
# simple double-quoted literal capture is unambiguous.
_WRAPPED_RE = re.compile(r"""wrapped\s*=\s*["']([A-Za-z0-9+/=]+)["']""")

# Anchor on the cloud.bs.master.key key, then its double-quoted default literal.
_DEFAULT_RE = re.compile(
    r'(\["cloud\.bs\.master\.key"\]\s*=\s*\{.*?default\s*=\s*)"([^"]*)"',
    re.DOTALL)


def parse_wrapped(text):
    """Return the base64 wrapped-master string from a bs_master.lua file."""
    m = _WRAPPED_RE.search(text)
    if not m:
        raise ValueError("no `wrapped = \"<base64>\"` found in bs_master.lua")
    blob = m.group(1)
    # base64 of (12B nonce + ≥1B ct + 16B tag) = ≥29 bytes → ≥40 b64 chars.
    if len(blob) < 40 or len(blob) % 4 != 0:
        raise ValueError("wrapped master is not a plausible base64 envelope")
    return blob


def rewrite_master_default(schema_text, wrapped):
    """Return schema_text with the cloud.bs.master.key default set to wrapped."""
    if not _DEFAULT_RE.search(schema_text):
        raise ValueError("cloud.bs.master.key default not found in schema")
    return _DEFAULT_RE.sub(lambda m: m.group(1) + '"' + wrapped + '"',
                           schema_text, count=1)


def extract_master_default(schema_text):
    """Return the cloud.bs.master.key default string from a schema."""
    m = _DEFAULT_RE.search(schema_text)
    if not m:
        raise ValueError("cloud.bs.master.key default not found in schema")
    return m.group(2)


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: gen_bs_master.py "
                         "<bs_master.lua> <installed cloud.lua>\n")
        return 2
    master_path, schema_path = argv[1], argv[2]
    try:
        with open(master_path, encoding="utf-8") as fh:
            wrapped = parse_wrapped(fh.read())
        with open(schema_path, encoding="utf-8") as fh:
            schema = fh.read()
        out = rewrite_master_default(schema, wrapped)
        with open(schema_path, "w", encoding="utf-8") as fh:
            fh.write(out)
    except (ValueError, OSError) as exc:
        sys.stderr.write("gen_bs_master: %s\n" % exc)
        return 2
    sys.stderr.write("gen_bs_master: seeded cloud.bs.master.key from %s\n"
                     % master_path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
