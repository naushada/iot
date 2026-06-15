#!/usr/bin/env python3
"""Build-time generator: bake a wifi_credentials.lua file into the wifi.networks
schema default.

Invoked from the iot recipe's do_install when an (optional, gitignored)
wifi_credentials.lua is present. Parses a constrained lua subset describing one
or more WiFi networks, converts it to the JSON shape the wifi-client daemon
consumes (parse_wifi_networks), and rewrites the `wifi.networks` `default = '…'`
line of the already-installed schemas/wifi.lua in place.

This is NOT a full lua interpreter — only the documented credential shapes are
accepted; anything else is a hard error so a bad file fails the build loudly
rather than shipping wrong credentials.

Usage:  gen_wifi_default.py <wifi_credentials.lua> <installed wifi.lua>

Credential file shapes:
    return { ssid = "cordoba_2G", password = "whatever" }       -- simple PSK
    return {                                                     -- explicit list
      { ssid = "A", key_mgmt = "WPA-PSK", psk = "p", priority = 10 },
      { ssid = "Guest", key_mgmt = "NONE" },
      { ssid = "Corp", key_mgmt = "WPA-EAP", identity = "u", password = "s" },
    }
"""

import json
import re
import sys

DEFAULT_PRIORITY = 10

# ───────────────────────────── lua subset parser ────────────────────────────

_TOKEN_RE = re.compile(r"""
      (?P<ws>\s+)
    | (?P<comment>--[^\n]*)
    | (?P<string>"(?:\\.|[^"\\])*" | '(?:\\.|[^'\\])*')
    | (?P<number>-?\d+)
    | (?P<name>[A-Za-z_][A-Za-z0-9_]*)
    | (?P<punct>[{}=,;])
""", re.VERBOSE)

_LUA_ESCAPES = {
    "n": "\n", "t": "\t", "r": "\r", "a": "\a", "b": "\b",
    "f": "\f", "v": "\v", "\\": "\\", '"': '"', "'": "'", "\n": "\n",
}


def _decode_lua_string(tok):
    """Strip the surrounding quotes and decode escapes; reject control chars."""
    body = tok[1:-1]
    out = []
    i = 0
    while i < len(body):
        c = body[i]
        if c == "\\" and i + 1 < len(body):
            nxt = body[i + 1]
            out.append(_LUA_ESCAPES.get(nxt, nxt))
            i += 2
        else:
            out.append(c)
            i += 1
    s = "".join(out)
    if any(ord(ch) < 0x20 or ord(ch) == 0x7F for ch in s):
        raise ValueError("control character in string value %r" % s)
    return s


def _tokenize(text):
    toks = []
    pos = 0
    while pos < len(text):
        m = _TOKEN_RE.match(text, pos)
        if not m:
            raise ValueError("unexpected character at offset %d: %r"
                             % (pos, text[pos:pos + 16]))
        pos = m.end()
        kind = m.lastgroup
        if kind in ("ws", "comment"):
            continue
        toks.append((kind, m.group()))
    return toks


class _Parser:
    def __init__(self, toks):
        self.toks = toks
        self.i = 0

    def _peek(self):
        return self.toks[self.i] if self.i < len(self.toks) else (None, None)

    def _next(self):
        t = self._peek()
        self.i += 1
        return t

    def _expect(self, value):
        kind, v = self._next()
        if v != value:
            raise ValueError("expected %r, got %r" % (value, v))

    def parse_program(self):
        kind, v = self._next()
        if v != "return":
            raise ValueError("credential file must start with 'return'")
        val = self._parse_value()
        return val

    def _parse_value(self):
        kind, v = self._peek()
        if kind == "string":
            self._next()
            return _decode_lua_string(v)
        if kind == "number":
            self._next()
            return int(v)
        if v == "{":
            return self._parse_table()
        raise ValueError("unexpected token %r" % (v,))

    def _parse_table(self):
        """Return {'fields': {name: value}, 'items': [value, ...]}."""
        self._expect("{")
        fields, items = {}, []
        while True:
            kind, v = self._peek()
            if v == "}":
                self._next()
                break
            if v is None:
                raise ValueError("unterminated table (missing '}')")
            if kind == "name" and self._lookahead_is_assign():
                self._next()              # name
                self._expect("=")
                fields[v] = self._parse_value()
            else:
                items.append(self._parse_value())
            kind, v = self._peek()
            if v in (",", ";"):
                self._next()
        return {"fields": fields, "items": items}

    def _lookahead_is_assign(self):
        return (self.i + 1 < len(self.toks)
                and self.toks[self.i + 1][1] == "=")


# ───────────────────────────── credential mapping ───────────────────────────

def _table_to_network(tbl, idx):
    if not isinstance(tbl, dict) or tbl.get("items"):
        raise ValueError("network entry %d is not a field table" % idx)
    f = tbl["fields"]

    ssid = f.get("ssid")
    if not isinstance(ssid, str) or not ssid:
        raise ValueError("network entry %d missing non-empty 'ssid'" % idx)

    key_mgmt = f.get("key_mgmt", "WPA-PSK")
    if not isinstance(key_mgmt, str):
        raise ValueError("network entry %d 'key_mgmt' must be a string" % idx)

    net = {"ssid": ssid, "key_mgmt": key_mgmt}

    if key_mgmt == "NONE":
        pass
    elif key_mgmt == "WPA-EAP":
        identity = f.get("identity")
        password = f.get("password")
        if not isinstance(identity, str) or not identity:
            raise ValueError("network entry %d (WPA-EAP) needs 'identity'" % idx)
        if not isinstance(password, str) or not password:
            raise ValueError("network entry %d (WPA-EAP) needs 'password'" % idx)
        net["identity"] = identity
        net["password"] = password
        for opt in ("eap", "phase2", "ca_cert"):
            if opt in f:
                if not isinstance(f[opt], str):
                    raise ValueError("network entry %d '%s' must be a string"
                                     % (idx, opt))
                net[opt] = f[opt]
    else:
        # PSK-like: credential from 'psk', or 'password' as a friendly alias.
        cred = f.get("psk", f.get("password"))
        if not isinstance(cred, str) or not cred:
            raise ValueError("network entry %d (key_mgmt=%s) needs 'psk' "
                             "or 'password'" % (idx, key_mgmt))
        net["psk"] = cred

    prio = f.get("priority", DEFAULT_PRIORITY)
    if not isinstance(prio, int):
        raise ValueError("network entry %d 'priority' must be an integer" % idx)
    net["priority"] = prio
    return net


def parse_credentials(text):
    """Parse wifi_credentials.lua text into a list of network dicts.

    Accepts either a single field table (one network) or a list of field
    tables. Raises ValueError on any shape outside the documented forms.
    """
    root = _Parser(_tokenize(text)).parse_program()
    if not isinstance(root, dict):
        raise ValueError("credential file must return a table")
    if root["items"]:
        if root["fields"]:
            raise ValueError("table mixes list entries and named fields")
        return [_table_to_network(item, i) for i, item in enumerate(root["items"])]
    if root["fields"]:
        return [_table_to_network(root, 0)]
    raise ValueError("credential table is empty")


def networks_to_json(networks):
    """Compact JSON array in the daemon's expected key order."""
    return json.dumps(networks, separators=(",", ":"), ensure_ascii=False)


# ──────────────────────────── schema rewriting ──────────────────────────────

# Anchor on the wifi.networks key, then its single-quoted default literal.
_DEFAULT_RE = re.compile(
    r"(\[\"wifi\.networks\"\]\s*=\s*\{.*?default\s*=\s*)'((?:\\.|[^'\\])*)'",
    re.DOTALL)


def _lua_sq_literal(s):
    """Embed s into a single-quoted lua string literal (escape \\ and ')."""
    return "'" + s.replace("\\", "\\\\").replace("'", "\\'") + "'"


def _lua_sq_unescape(s):
    out = []
    i = 0
    while i < len(s):
        if s[i] == "\\" and i + 1 < len(s):
            out.append(s[i + 1])
            i += 2
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def rewrite_wifi_networks_default(schema_text, json_text):
    """Return schema_text with the wifi.networks default replaced by json_text."""
    if not _DEFAULT_RE.search(schema_text):
        raise ValueError("wifi.networks default not found in schema")
    literal = _lua_sq_literal(json_text)
    return _DEFAULT_RE.sub(lambda m: m.group(1) + literal, schema_text, count=1)


def extract_wifi_networks_default(schema_text):
    """Return the (un-escaped) wifi.networks default string from a schema."""
    m = _DEFAULT_RE.search(schema_text)
    if not m:
        raise ValueError("wifi.networks default not found in schema")
    return _lua_sq_unescape(m.group(2))


# ───────────────────────────────── CLI ──────────────────────────────────────

def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: gen_wifi_default.py "
                         "<wifi_credentials.lua> <installed wifi.lua>\n")
        return 2
    cred_path, schema_path = argv[1], argv[2]
    try:
        with open(cred_path, encoding="utf-8") as fh:
            nets = parse_credentials(fh.read())
        json_text = networks_to_json(nets)
        with open(schema_path, encoding="utf-8") as fh:
            schema = fh.read()
        out = rewrite_wifi_networks_default(schema, json_text)
        with open(schema_path, "w", encoding="utf-8") as fh:
            fh.write(out)
    except (ValueError, OSError) as exc:
        sys.stderr.write("gen_wifi_default: %s\n" % exc)
        return 2
    sys.stderr.write("gen_wifi_default: seeded wifi.networks from %s -> %s\n"
                     % (cred_path, json_text))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
