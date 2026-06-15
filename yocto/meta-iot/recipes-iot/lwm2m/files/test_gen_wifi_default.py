#!/usr/bin/env python3
"""Unit tests for gen_wifi_default.py — the build-time generator that turns a
wifi_credentials.lua file into the wifi.networks schema default.

Pure-Python, no toolchain: run with
    python3 -m unittest discover -s <this dir> -p 'test_gen_wifi_default.py'
"""

import json
import unittest

import gen_wifi_default as g


# A minimal stand-in for schemas/wifi.lua with the wifi.networks default plus
# two sibling keys whose `default =` lines MUST stay untouched by the rewrite.
SCHEMA = '''return {
  namespace = "wifi",
  keys = {
    ["wifi.iface"]    = { access = "Admin", type = "string", default = "wlan0" },
    ["wifi.networks"] = {
        access  = "Admin", type = "string",
        default = '[{"ssid":"changeme","key_mgmt":"WPA-PSK","psk":"changeme","priority":10}]' },
    ["wifi.dhcp.path"] = { access = "Admin", type = "string", default = "" },
  },
}
'''


class ParseCredentials(unittest.TestCase):
    def test_simple_psk_password_maps_to_psk(self):
        nets = g.parse_credentials(
            'return { ssid = "cordoba_2G", password = "whatever" }')
        self.assertEqual(nets, [{
            "ssid": "cordoba_2G", "key_mgmt": "WPA-PSK",
            "psk": "whatever", "priority": 10,
        }])

    def test_explicit_psk_field(self):
        nets = g.parse_credentials(
            'return { ssid = "AP", key_mgmt = "WPA-PSK", psk = "p", priority = 3 }')
        self.assertEqual(nets[0]["psk"], "p")
        self.assertEqual(nets[0]["priority"], 3)

    def test_list_of_networks_preserves_order(self):
        nets = g.parse_credentials('''return {
            { ssid = "A", psk = "a", priority = 5 },
            { ssid = "B", psk = "b" },
        }''')
        self.assertEqual([n["ssid"] for n in nets], ["A", "B"])
        self.assertEqual(nets[0]["priority"], 5)
        self.assertEqual(nets[1]["priority"], 10)  # defaulted

    def test_open_network_none(self):
        nets = g.parse_credentials('return { ssid = "Guest", key_mgmt = "NONE" }')
        self.assertEqual(nets[0]["key_mgmt"], "NONE")
        self.assertNotIn("psk", nets[0])
        self.assertNotIn("password", nets[0])

    def test_eap_retains_identity_password(self):
        nets = g.parse_credentials('''return {
            ssid = "Corp", key_mgmt = "WPA-EAP",
            identity = "u@corp", password = "secret", eap = "PEAP" }''')
        n = nets[0]
        self.assertEqual(n["key_mgmt"], "WPA-EAP")
        self.assertEqual(n["identity"], "u@corp")
        self.assertEqual(n["password"], "secret")
        self.assertEqual(n["eap"], "PEAP")
        self.assertNotIn("psk", n)

    def test_eap_omits_eap_when_absent(self):
        nets = g.parse_credentials('''return {
            ssid = "Corp", key_mgmt = "WPA-EAP",
            identity = "u", password = "p" }''')
        self.assertNotIn("eap", nets[0])
        self.assertNotIn("phase2", nets[0])

    def test_ignores_comments(self):
        nets = g.parse_credentials('''-- my home AP
            return { ssid = "Home", password = "pw" }  -- trailing comment
        ''')
        self.assertEqual(nets[0]["ssid"], "Home")

    def test_double_quoted_and_single_quoted_values(self):
        nets = g.parse_credentials("return { ssid = 'A', psk = \"b\" }")
        self.assertEqual(nets[0]["ssid"], "A")
        self.assertEqual(nets[0]["psk"], "b")

    # ── hard errors (fail the build loudly) ──
    def test_reject_missing_ssid(self):
        with self.assertRaises(ValueError):
            g.parse_credentials('return { psk = "x" }')

    def test_reject_empty_ssid(self):
        with self.assertRaises(ValueError):
            g.parse_credentials('return { ssid = "", psk = "x" }')

    def test_reject_psk_without_credential(self):
        with self.assertRaises(ValueError):
            g.parse_credentials('return { ssid = "A" }')

    def test_reject_eap_without_identity(self):
        with self.assertRaises(ValueError):
            g.parse_credentials(
                'return { ssid = "C", key_mgmt = "WPA-EAP", password = "p" }')

    def test_reject_eap_without_password(self):
        with self.assertRaises(ValueError):
            g.parse_credentials(
                'return { ssid = "C", key_mgmt = "WPA-EAP", identity = "u" }')

    def test_reject_no_return(self):
        with self.assertRaises(ValueError):
            g.parse_credentials('{ ssid = "A", psk = "b" }')

    def test_reject_control_char_in_value(self):
        with self.assertRaises(ValueError):
            g.parse_credentials('return { ssid = "A\\npsk", psk = "b" }')


class NetworksToJson(unittest.TestCase):
    def test_compact_and_valid(self):
        nets = g.parse_credentials('return { ssid = "A", password = "b" }')
        s = g.networks_to_json(nets)
        self.assertNotIn(" ", s)            # compact
        self.assertEqual(json.loads(s), nets)

    def test_field_order_matches_daemon(self):
        nets = g.parse_credentials('return { ssid = "A", password = "b" }')
        s = g.networks_to_json(nets)
        self.assertTrue(s.startswith('[{"ssid":"A","key_mgmt":"WPA-PSK","psk":"b"'))


class RewriteSchema(unittest.TestCase):
    def test_replaces_only_wifi_networks(self):
        new_json = g.networks_to_json(
            g.parse_credentials('return { ssid = "cordoba_2G", password = "pw" }'))
        out = g.rewrite_wifi_networks_default(SCHEMA, new_json)
        # sibling keys untouched
        self.assertIn('default = "wlan0"', out)
        self.assertIn('default = "" }', out)
        # exactly one wifi.networks default, and it's the new one
        self.assertNotIn("changeme", out)
        self.assertEqual(g.extract_wifi_networks_default(out), new_json)

    def test_escaping_roundtrip(self):
        # ssid/psk with backslash, single quote, double quote must survive the
        # single-quoted lua literal byte-exact.
        nets = [{"ssid": "A'B\\C", "key_mgmt": "WPA-PSK",
                 "psk": 'p"q', "priority": 10}]
        new_json = g.networks_to_json(nets)
        out = g.rewrite_wifi_networks_default(SCHEMA, new_json)
        self.assertEqual(g.extract_wifi_networks_default(out), new_json)
        self.assertEqual(json.loads(g.extract_wifi_networks_default(out)), nets)

    def test_rewrite_missing_key_raises(self):
        with self.assertRaises(ValueError):
            g.rewrite_wifi_networks_default('return { keys = {} }', "[]")


if __name__ == "__main__":
    unittest.main()
