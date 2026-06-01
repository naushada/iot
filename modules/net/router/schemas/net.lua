-- net.* schema for net-router (L13).
--
-- Read keys (operator → daemon):
--   net.tun.dev               - tun interface to monitor (default "tun0")
--   net.lwm2m.target.ip       - DNAT target (required; lwm2m client IP)
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
    ["net.tun.dev"]               = { type = "string",  default = "tun0" },
    ["net.lwm2m.target.ip"]       = { type = "string"  },
    ["net.lwm2m.target.port"]     = { type = "integer", default = 5684,
                                      min = 1, max = 65535 },
    ["net.iface.priority"]        = { type = "string",  default = "eth,wifi,cellular" },
    ["net.iface.eth.name"]        = { type = "string",  default = "eth0" },
    ["net.iface.wifi.name"]       = { type = "string",  default = "wlan0" },
    ["net.iface.cellular.name"]   = { type = "string",  default = "wwan0" },
    ["net.forward.ports"]         = { type = "string",  default = "80,443,5684" },
    ["net.custom.rules"]          = { type = "string",  default = "[]" },
    ["net.poll.interval.sec"]     = { type = "integer", default = 5,
                                      min = 1, max = 600 },

    -- ───────── Write keys (daemon → operator) ─────────
    ["net.state"]                 = { type = "string" },
    ["net.tun.ip"]                = { type = "string" },
    ["net.tun.gateway"]           = { type = "string" },
    ["net.iface.active"]          = { type = "string" },
    ["net.rules.applied.count"]   = { type = "integer", min = 0 },
    ["net.last.apply.unix"]       = { type = "integer", min = 0 },
  },
}
