-- cloud.* schema (L21/D1).
--
-- Multi-tenant cloud server state.  Endpoints are stored as a
-- single JSON array (like wifi.networks) so the schema can validate
-- the key regardless of how many devices are provisioned.
--
-- cloud.endpoints = [
--   {
--     "endpoint":      "urn:dev:gateway-42",
--     "state":         "online",
--     "tun_ip":        "10.9.0.12",
--     "proxy_port":    5001,
--     "registered":    true,
--     "last_seen_unix": 1718123456
--   }
-- ]
--
-- Install at /etc/iot/ds-schemas/cloud.lua (ds-server auto-loads).

return {
  namespace = "cloud",
  keys = {
    -- JSON array of provisioned endpoints (read by cloud UI,
    -- written by cloud server daemon).
    ["cloud.endpoints"] = {
      type    = "string",
      default = "[]",
    },

    -- VPN subnet for tunnel IP allocation.
    ["cloud.vpn.subnet"] = {
      type    = "string",
      default = "10.9.0.0/24",
    },

    -- Next available proxy port (bump-counter).
    ["cloud.vpn.port.next"] = {
      type    = "integer",
      default = 5001,
      min     = 5001,
      max     = 6000,
    },
  },
}
