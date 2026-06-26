#!/usr/bin/env python3
"""Unit tests for gen_bs_psk.py — the host-side per-unit Bootstrap PSK
derivation used by zero-touch provisioning.

Pure-Python, no toolchain:
    python3 -m unittest discover -s <this dir> -p 'test_gen_bs_psk.py'
"""

import os
import tempfile
import unittest

import gen_bs_psk as g


class Hkdf(unittest.TestCase):
    """RFC 5869 test vectors — proves the HKDF primitive itself is correct."""

    def test_rfc5869_case1(self):
        ikm = bytes.fromhex("0b" * 22)
        salt = bytes.fromhex("000102030405060708090a0b0c")
        info = bytes.fromhex("f0f1f2f3f4f5f6f7f8f9")
        self.assertEqual(
            g.hkdf_sha256(ikm, salt, info, 42).hex(),
            "3cb25f25faacd57a90434f64d0362f2a"
            "2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
            "34007208d5b887185865")

    def test_rfc5869_case3_zero_len_salt_and_info(self):
        ikm = bytes.fromhex("0b" * 22)
        self.assertEqual(
            g.hkdf_sha256(ikm, b"", b"", 42).hex(),
            "8da4e775a563c18f715f802a063c5a31"
            "b8a11f5c5ee1879ec3454e5f3c738d2d"
            "9d201395faa4b61a96c8")

    def test_empty_salt_equals_hashlen_zeros(self):
        ikm, info = bytes.fromhex("0b0b0b0b0b0b0b0b"), b"ctx"
        self.assertEqual(
            g.hkdf_sha256(ikm, b"\x00" * 32, info, 32),
            g.hkdf_sha256(ikm, b"", info, 32))


class DeriveBsPsk(unittest.TestCase):

    # Shared VERBATIM with apps/test/psk_gen_test.cpp
    # (DeriveBsPsk.MatchesSharedCrossCheckVector). Do not change one side
    # without the other — the C++ cloud server and this host tool MUST agree.
    MASTER_HEX = ("000102030405060708090a0b0c0d0e0f"
                  "101112131415161718191a1b1c1d1e1f")
    SERIAL = "100000003d1f9c2e"
    EXPECT = ("223a82da7acb983c1372ec5e72c77d00"
              "8fc40281e737bb4aea689f53600d4fe5")

    def test_shared_cross_check_vector(self):
        self.assertEqual(
            g.derive_bs_psk_hex(self.MASTER_HEX, self.SERIAL), self.EXPECT)

    def test_deterministic_and_serial_bound(self):
        m = "ab" * 32
        self.assertEqual(g.derive_bs_psk_hex(m, "A"),
                         g.derive_bs_psk_hex(m, "A"))
        self.assertNotEqual(g.derive_bs_psk_hex(m, "A"),
                            g.derive_bs_psk_hex(m, "B"))
        self.assertEqual(len(g.derive_bs_psk_hex(m, "A")), 64)

    def test_case_insensitive_master(self):
        self.assertEqual(
            g.derive_bs_psk_hex(self.MASTER_HEX.upper(), self.SERIAL),
            self.EXPECT)

    def test_bad_master_raises(self):
        for bad in ("", "abc", "zz", "12g4"):
            with self.assertRaises(ValueError):
                g.derive_bs_psk_hex(bad, "serial")

    def test_empty_serial_raises(self):
        with self.assertRaises(ValueError):
            g.derive_bs_psk_hex(self.MASTER_HEX, "  ")


class LoadMaster(unittest.TestCase):

    def test_literal_hex_passthrough(self):
        self.assertEqual(g.load_master("deadbeef"), "deadbeef")

    def test_reads_bs_master_lua(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "bs_master.lua")
            with open(p, "w", encoding="utf-8") as fh:
                fh.write('return { master = "00ff00ff" }\n')
            self.assertEqual(g.load_master(p), "00ff00ff")

    def test_lua_without_master_raises(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "bs_master.lua")
            with open(p, "w", encoding="utf-8") as fh:
                fh.write('return { nope = "x" }\n')
            with self.assertRaises(ValueError):
                g.load_master(p)


class Cli(unittest.TestCase):

    def test_main_prints_derived_key(self):
        import io
        from contextlib import redirect_stdout
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = g.main(["gen_bs_psk.py", DeriveBsPsk.MASTER_HEX,
                         DeriveBsPsk.SERIAL])
        self.assertEqual(rc, 0)
        self.assertEqual(buf.getvalue().strip(), DeriveBsPsk.EXPECT)

    def test_main_bad_args(self):
        self.assertEqual(g.main(["gen_bs_psk.py"]), 2)


if __name__ == "__main__":
    unittest.main()
