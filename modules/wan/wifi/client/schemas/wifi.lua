-- wifi.* schema for wifi-client (L15).
--
-- Read keys (operator → daemon):
--   wifi.iface              - kernel iface to manage (default "wlan0")
--   wifi.ctrl.dir           - wpa_supplicant control-socket dir
--                             (default "/run/wpa_supplicant")
--   wifi.wpa.path           - path to wpa_supplicant(8)
--                             (default "/usr/sbin/wpa_supplicant")
--   wifi.networks           - JSON array of network entries (validated in
--                             code, not lua). Two shapes per entry:
--                               PSK : {ssid, psk, priority, key_mgmt}
--                                     key_mgmt "WPA-PSK" (default) or "NONE"
--                               EAP : {ssid, key_mgmt="WPA-EAP", eap,
--                                     identity, password, phase2, ca_cert,
--                                     priority}  (WPA-Enterprise; eap
--                                     defaults "PEAP", phase2 "auth=MSCHAPV2")
--                             Default seeds one placeholder PSK network so a
--                             fresh image auto-associates once provisioned;
--                             operators override via ds-cli / cloud-ui.
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
--   wifi.dhcp.mask          - subnet mask of the lease (udhcpc $subnet)
--   wifi.dhcp.gateway       - default gateway(s) (udhcpc $router)
--   wifi.dhcp.dns           - nameserver(s), space-separated (udhcpc $dns)
--   wifi.dhcp.lease.sec     - lease time in seconds (udhcpc $lease)
--   wifi.dhcp.domain        - DNS domain (udhcpc $domain)
--   wifi.dhcp.obtained.unix - unix time the lease was bound (UI countdown)
--                             wifi.dhcp.{mask,gateway,dns,lease.sec,domain,
--                             obtained.unix} are written by the udhcpc hook
--                             (udhcpc-ds.script) via ds-cli, not the daemon.
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
    ["wifi.iface"]              = {
        access  = "Admin", type = "string",  default = "wlan0" },
    ["wifi.ctrl.dir"]           = {
        access  = "Admin", type = "string",
                                    default = "/run/wpa_supplicant" },
    ["wifi.wpa.path"]           = {
        access  = "Admin", type = "string",
                                    default = "/usr/sbin/wpa_supplicant" },
    -- Default seeds one placeholder PSK network. wifi-client auto-starts
    -- on boot (SYSTEMD_AUTO_ENABLE=enable) and reads this default until an
    -- operator overrides wifi.networks. Replace ssid/psk per deployment;
    -- the placeholder parks the daemon in "disconnected" (no such AP) until
    -- then. NOTE: this value lives in the world-readable image schema — do
    -- not commit a real secret here; provision per-device for production.
    ["wifi.networks"]           = {
        access  = "Admin", type = "string",
        default = '[{"ssid":"changeme","key_mgmt":"WPA-PSK","psk":"changeme","priority":10}]' },
    ["wifi.scan.interval.sec"]  = {
        access  = "Admin", type = "integer", default = 60,
                                    min = 0, max = 86400 },
    ["wifi.scan.max.results"]   = {
        access  = "Admin", type = "integer", default = 20,
                                    min = 1, max = 200 },
    ["wifi.scan.request"]       = {
        access  = "Admin", type = "integer", default = 0,
                                    min = 0 },
    ["wifi.dhcp.client"]        = {
        access  = "Admin", type = "string",  default = "auto" },
    ["wifi.dhcp.path"]          = {
        access  = "Admin", type = "string",  default = "" },

    -- ───────── Write keys (daemon → operator) ─────────
    ["wifi.assoc.state"]        = {
        access  = "Viewer", type = "string" },
    ["wifi.assoc.ssid"]         = {
        access  = "Viewer", type = "string" },
    ["wifi.assoc.bssid"]        = {
        access  = "Viewer", type = "string" },
    ["wifi.signal.rssi"]        = {
        access  = "Viewer", type = "integer" },
    ["wifi.scan.results"]       = {
        access  = "Viewer", type = "string" },
    ["wifi.scan.last.unix"]     = {
        access  = "Viewer", type = "integer", min = 0 },
    ["wifi.dhcp.state"]         = {
        access  = "Viewer", type = "string" },
    ["wifi.dhcp.ip"]            = {
        access  = "Viewer", type = "string" },
    -- Lease detail mirrored from the udhcpc hook (udhcpc-ds.script) so the
    -- device-iot UI can render the full network config. Written by the hook
    -- via ds-cli, not by the daemon.
    ["wifi.dhcp.mask"]          = {
        access  = "Viewer", type = "string" },
    ["wifi.dhcp.gateway"]       = {
        access  = "Viewer", type = "string" },
    ["wifi.dhcp.dns"]           = {
        access  = "Viewer", type = "string" },
    ["wifi.dhcp.lease.sec"]     = {
        access  = "Viewer", type = "integer", min = 0 },
    ["wifi.dhcp.domain"]        = {
        access  = "Viewer", type = "string" },
    ["wifi.dhcp.obtained.unix"] = {
        access  = "Viewer", type = "integer", min = 0 },
    ["wifi.pid.wpa"]            = {
        access  = "Viewer", type = "integer", min = 0 },
    ["wifi.pid.dhcp"]           = {
        access  = "Viewer", type = "integer", min = 0 },
    ["wifi.last.error"]         = {
        access  = "Viewer", type = "string" },
  },
}
