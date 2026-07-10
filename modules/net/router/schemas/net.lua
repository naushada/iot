-- net.* schema for net-router (L13).
--
-- Read keys (operator → daemon):
--   net.tun.dev               - tun interface to monitor (default "tun0")
--   net.lwm2m.target.ip       - DNAT target (optional; lwm2m client IP). When
--                               unset the router still runs — it just skips the
--                               port-forward/DNAT rules until a target is given.
--   net.lwm2m.target.port     - DNAT target port (default 5684)
--   net.iface.priority        - comma-joined outgoing priority list
--                               (default "eth,wifi,cellular")
--   net.iface.eth.name        - kernel name for the eth slot (default "eth0")
--   net.iface.wifi.name       - kernel name for the wifi slot (default "wlan0")
--   net.iface.cellular.name   - kernel name for the cellular slot (default "wwan0")
--   net.forward.ports         - comma-joined ports to DNAT to net.lwm2m.target.ip
--                               (default "80,443,5684" — http, https, lwm2m/CoAP)
--   net.custom.rules          - JSON array of {action, proto, dport, ...} (default "[]")
--   net.poll.interval.sec     - iface-state poll cadence (default 5)
--
-- Write keys (daemon → operator):
--   net.state                 - "monitoring"/"installing"/"bad_custom_rules"/"error"/"exited"
--   net.tun.ip                - current IP on net.tun.dev (mirrors vpn.assigned.ip)
--   net.tun.gateway           - current gateway on the tun
--   net.iface.active          - highest-priority interface currently OPER UP
--   net.iface.active.ip       - that interface's routable IPv4 (LAN IP); the
--                               LwM2M /4/0/4 hook publishes it as the device's
--                               LAN IP without re-enumerating interfaces
--   net.rules.applied.count   - count of nft rules installed by this daemon
--   net.last.apply.unix       - unix timestamp of last successful `nft -f -`
--
-- Install at /etc/iot/ds-schemas/net.lua (ds-server auto-loads).
--
-- Convention: every key segment is dot-separated, no underscores in
-- segment names. C++-side identifiers (`lwm2m_target_ip`, etc.) keep
-- underscores because dots are illegal there.
--
-- Note: net.custom.rules is schema-typed as "string"; JSON shape is
-- validated in code (the schema can't json-parse).

return {
  namespace = "net",
  keys = {
    -- ───────── Read keys (operator → daemon) ─────────
    ["net.tun.dev"]               = {
        access  = "Admin", type = "string",  default = "tun0" },
    ["net.lwm2m.target.ip"]       = {
        access  = "Admin", type = "string"  },
    ["net.lwm2m.target.port"]     = {
        access  = "Admin", type = "integer", default = 5684,
                                      min = 1, max = 65535 },
    ["net.iface.priority"]        = {
        access  = "Admin", type = "string",  default = "eth,wifi,cellular" },
    ["net.iface.eth.name"]        = {
        access  = "Admin", type = "string",  default = "eth0" },
    ["net.iface.wifi.name"]       = {
        access  = "Admin", type = "string",  default = "wlan0" },
    -- The WP7702 module owns the data session internally; the host cannot open
    -- a data call on wwan0 (firmware refuses it). Cellular WAN reaches us over
    -- the modem's ECM link (cdc_ether), which enumerates as eth1 on the RPi3B —
    -- eth0 is the onboard smsc95xx. See apps/docs/hw-bringup-wp7702-cellular-wan.md.
    ["net.iface.cellular.name"]   = {
        access  = "Admin", type = "string",  default = "eth1" },
    ["net.forward.ports"]         = {
        access  = "Admin", type = "string",  default = "80,443,5684" },
    ["net.custom.rules"]          = {
        access  = "Admin", type = "string",  default = "[]" },
    ["net.poll.interval.sec"]     = {
        access  = "Admin", type = "integer", default = 5,
                                      min = 1, max = 600 },

    -- ───────── Write keys (daemon → operator) ─────────
    ["net.state"]                 = {
        access  = "Viewer", type = "string" },
    ["net.tun.ip"]                = {
        access  = "Viewer", type = "string" },
    ["net.tun.gateway"]           = {
        access  = "Viewer", type = "string" },
    ["net.iface.active"]          = {
        access  = "Viewer", type = "string" },
    ["net.iface.active.ip"]       = {
        access  = "Viewer", type = "string" },
    ["net.rules.applied.count"]   = {
        access  = "Viewer", type = "integer", min = 0 },
    ["net.last.apply.unix"]       = {
        access  = "Viewer", type = "integer", min = 0 },
  },
}
