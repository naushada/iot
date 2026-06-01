-- wifi.* schema for wifi-client (L15).
--
-- Read keys (operator → daemon):
--   wifi.iface              - kernel iface to manage (default "wlan0")
--   wifi.ctrl.dir           - wpa_supplicant control-socket dir
--                             (default "/run/wpa_supplicant")
--   wifi.wpa.path           - path to wpa_supplicant(8)
--                             (default "/usr/sbin/wpa_supplicant")
--   wifi.networks           - JSON array of {ssid, psk, priority, key_mgmt}
--                             entries (default "[]"; validated in code, not lua)
--   wifi.scan.interval.sec  - periodic background scan cadence in seconds
--                             (default 60; 0 disables periodic scans)
--   wifi.scan.max.results   - cap on the length of wifi.scan.results
--                             (default 20, range 1..200)
--   wifi.scan.request       - bump (any change) to trigger an immediate scan
--                             (default 0; mirrors net.iface.* "bump-counter"
--                             convention)
--   wifi.dhcp.client        - "udhcpc" / "dhclient" / "auto" (default "auto";
--                             "auto" probes udhcpc first, dhclient second)
--   wifi.dhcp.path          - override DHCP-client binary path; empty = use
--                             the default for whichever client was chosen
--
-- Write keys (daemon → operator):
--   wifi.assoc.state        - "disconnected" / "scanning" / "associating" /
--                             "4way" / "connected" / "conflict" / "exited"
--   wifi.assoc.ssid         - SSID of the AP we're associated to; empty when
--                             not connected
--   wifi.assoc.bssid        - BSSID (AP MAC) of the current association
--   wifi.signal.rssi        - latest signal level in dBm (integer);
--                             coalesced to one write per 5 s (NFR-WIFI-002)
--   wifi.scan.results       - JSON array [{ssid, bssid, signal, flags}, ...]
--                             ordered strongest-signal-first, truncated to
--                             wifi.scan.max.results
--   wifi.scan.last.unix     - unix timestamp of the most recent scan result
--                             write
--   wifi.dhcp.state         - "idle" / "requesting" / "bound" / "rebinding"
--                             / "exited"
--   wifi.dhcp.ip            - IPv4 leased on the iface (empty when not bound)
--   wifi.pid.wpa            - live wpa_supplicant pid (0 when not running)
--   wifi.pid.dhcp           - live DHCP-client pid (0 when not running)
--   wifi.last.error         - last non-fatal error message (auth reject,
--                             scan fail, bad_networks_json: ...)
--
-- Install at /etc/iot/ds-schemas/wifi.lua (ds-server auto-loads).
--
-- Convention: every key segment is dot-separated, no underscores in segment
-- names. Same as vpn.lua + net.lua. C++-side identifiers (parsed_networks,
-- scan_results_json, ...) keep underscores because dots are illegal there.
--
-- Note: wifi.networks is schema-typed as "string"; the JSON shape is
-- validated in code (nlohmann::json). Bad shape surfaces as
-- wifi.assoc.state="conflict" + wifi.last.error="bad_networks_json: ...".
-- The schema can't json-parse, hence the in-code check at start-of-cycle.
--
-- Note: paths (wpa.path, dhcp.path) are STRING-validated here. Filesystem
-- existence is checked by the daemon at spawn time, not at set time
-- (the schema can't stat() the host filesystem).

return {
  namespace = "wifi",
  keys = {
    -- ───────── Read keys (operator → daemon) ─────────
    ["wifi.iface"]              = { type = "string",  default = "wlan0" },
    ["wifi.ctrl.dir"]           = { type = "string",
                                    default = "/run/wpa_supplicant" },
    ["wifi.wpa.path"]           = { type = "string",
                                    default = "/usr/sbin/wpa_supplicant" },
    ["wifi.networks"]           = { type = "string",  default = "[]" },
    ["wifi.scan.interval.sec"]  = { type = "integer", default = 60,
                                    min = 0, max = 86400 },
    ["wifi.scan.max.results"]   = { type = "integer", default = 20,
                                    min = 1, max = 200 },
    ["wifi.scan.request"]       = { type = "integer", default = 0,
                                    min = 0 },
    ["wifi.dhcp.client"]        = { type = "string",  default = "auto" },
    ["wifi.dhcp.path"]          = { type = "string",  default = "" },

    -- ───────── Write keys (daemon → operator) ─────────
    ["wifi.assoc.state"]        = { type = "string" },
    ["wifi.assoc.ssid"]         = { type = "string" },
    ["wifi.assoc.bssid"]        = { type = "string" },
    ["wifi.signal.rssi"]        = { type = "integer" },
    ["wifi.scan.results"]       = { type = "string" },
    ["wifi.scan.last.unix"]     = { type = "integer", min = 0 },
    ["wifi.dhcp.state"]         = { type = "string" },
    ["wifi.dhcp.ip"]            = { type = "string" },
    ["wifi.pid.wpa"]            = { type = "integer", min = 0 },
    ["wifi.pid.dhcp"]           = { type = "integer", min = 0 },
    ["wifi.last.error"]         = { type = "string" },
  },
}
