#!/usr/bin/env python3
"""Tests for iot-bs-personalize — the flash-time per-unit BS PSK seed emitter.

The tool has no .py extension (it's a CLI), so we load it via importlib and also
exercise its CLI through subprocess.

    python3 -m unittest discover -s <this dir> -p 'test_iot_bs_personalize.py'
"""

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from importlib.machinery import SourceFileLoader

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "iot-bs-personalize")

sys.path.insert(0, HERE)          # so its `import gen_bs_psk` resolves
# The tool has no .py extension, so give importlib an explicit source loader.
loader = SourceFileLoader("iot_bs_personalize", TOOL)
spec = importlib.util.spec_from_loader(loader.name, loader)
ibp = importlib.util.module_from_spec(spec)
loader.exec_module(ibp)

# Shared cross-check vector (psk_gen_test.cpp / test_gen_bs_psk.py).
MASTER = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
SERIAL = "100000003d1f9c2e"
KEY = "223a82da7acb983c1372ec5e72c77d008fc40281e737bb4aea689f53600d4fe5"


class BuildSeed(unittest.TestCase):
    def test_matches_cross_check_vector(self):
        self.assertEqual(ibp.build_seed(MASTER, SERIAL),
                         {"serial": SERIAL, "key": KEY})

    def test_trims_serial(self):
        self.assertEqual(ibp.build_seed(MASTER, "  " + SERIAL + " ")["serial"],
                         SERIAL)

    def test_bad_master_raises(self):
        with self.assertRaises(ValueError):
            ibp.build_seed("zz", SERIAL)


class Cli(unittest.TestCase):
    def _run(self, args):
        return subprocess.run([sys.executable, TOOL] + args,
                              capture_output=True, text=True, cwd=HERE)

    def test_stdout_single_line_json(self):
        r = self._run([MASTER, SERIAL])
        self.assertEqual(r.returncode, 0)
        line = r.stdout.strip()
        self.assertNotIn("\n", line)                 # single line for sed
        self.assertEqual(json.loads(line), {"serial": SERIAL, "key": KEY})

    def test_writes_output_file(self):
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "bs-seed.json")
            r = self._run([MASTER, SERIAL, "-o", out])
            self.assertEqual(r.returncode, 0)
            with open(out, encoding="utf-8") as fh:
                self.assertEqual(json.load(fh), {"serial": SERIAL, "key": KEY})

    def test_master_from_file(self):
        with tempfile.TemporaryDirectory() as d:
            mf = os.path.join(d, "master.hex")
            with open(mf, "w", encoding="utf-8") as fh:
                fh.write(MASTER + "\n")
            r = self._run(["@" + mf, SERIAL])
            self.assertEqual(r.returncode, 0)
            self.assertEqual(json.loads(r.stdout), {"serial": SERIAL, "key": KEY})

    def test_bad_args(self):
        self.assertEqual(self._run([MASTER]).returncode, 2)


if __name__ == "__main__":
    unittest.main()
