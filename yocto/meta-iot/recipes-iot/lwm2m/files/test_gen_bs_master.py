#!/usr/bin/env python3
"""Unit tests for gen_bs_master.py — the build-time generator that bakes a
wrapped BS HKDF master into the cloud.bs.master.key schema default.

Pure-Python, no toolchain:
    python3 -m unittest discover -s <this dir> -p 'test_gen_bs_master.py'
"""

import unittest

import gen_bs_master as g


# Minimal stand-in for schemas/cloud.lua: the cloud.bs.master.key default plus
# sibling keys whose `default =` lines MUST stay untouched by the rewrite.
SCHEMA = '''return {
  namespace = "cloud",
  keys = {
    ["cloud.bs.psk.key"]    = { access = "Admin", type = "opaque", default = "" },
    ["cloud.bs.master.key"] = {
        access = "Admin", type = "opaque", default = "",
        write_acl = {"gid:cloud-svc"}, read_acl = {"gid:cloud-svc"} },
    ["cloud.dm.psk.key"]    = { access = "Admin", type = "opaque", default = "keepme" },
  },
}
'''

# A plausible base64 envelope (the deterministic test vector from
# apps/test/psk_gen_test.cpp BsMasterEnvelope — base64 of nonce||ct||tag).
BLOB = ("CwsLCwsLCwsLCwsLvSPRai0N5R/JTXdybWG5"
        "SZ/lsc2/iF2o+2QR4pd/067Rwn3QtVkJvc/fXbVrVwBb")


class ParseWrapped(unittest.TestCase):
    def test_extracts_blob(self):
        self.assertEqual(
            g.parse_wrapped('return { wrapped = "%s" }' % BLOB), BLOB)

    def test_single_quotes_ok(self):
        self.assertEqual(
            g.parse_wrapped("return { wrapped = '%s' }" % BLOB), BLOB)

    def test_missing_wrapped_raises(self):
        with self.assertRaises(ValueError):
            g.parse_wrapped('return { nope = "x" }')

    def test_short_or_misaligned_blob_raises(self):
        with self.assertRaises(ValueError):
            g.parse_wrapped('return { wrapped = "AAAA" }')        # too short
        with self.assertRaises(ValueError):
            g.parse_wrapped('return { wrapped = "%s" }' % (BLOB[:-1]))  # not %4


class RewriteDefault(unittest.TestCase):
    def test_rewrites_only_master_default(self):
        out = g.rewrite_master_default(SCHEMA, BLOB)
        self.assertEqual(g.extract_master_default(out), BLOB)
        # siblings untouched
        self.assertIn('["cloud.bs.psk.key"]    = { access = "Admin", '
                      'type = "opaque", default = "" }', out)
        self.assertIn('default = "keepme"', out)

    def test_is_idempotent(self):
        once = g.rewrite_master_default(SCHEMA, BLOB)
        twice = g.rewrite_master_default(once, BLOB)
        self.assertEqual(once, twice)

    def test_missing_key_raises(self):
        with self.assertRaises(ValueError):
            g.rewrite_master_default('return { keys = {} }', BLOB)


class Cli(unittest.TestCase):
    def test_main_bad_args(self):
        self.assertEqual(g.main(["gen_bs_master.py"]), 2)


if __name__ == "__main__":
    unittest.main()
