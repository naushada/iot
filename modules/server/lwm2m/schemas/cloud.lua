-- cloud.* schema (L21/D1).
--
-- Multi-tenant cloud server state: endpoint registry, VPN tunnel
-- assignments, and proxy-port mappings.  The cloud's iot-httpd
-- reads these keys at startup; the cloud server daemon writes
-- them at device bootstrap / deprovision time.
--
-- Install at /etc/iot/ds-schemas/cloud.lua (ds-server auto-loads).

return {
  namespace = "cloud",
  keys = {
    -- JSON array of provisioned endpoint IDs.
    ["cloud.endpoints"] = {
      type    = "string",
      default = "[]",
    },

    -- Per-endpoint state keys.  Written by the cloud server when a
    -- device bootstraps or its tunnel state changes.
    ["cloud.ep.state"] = {
      type    = "string",
      default = "offline",   -- "offline" | "bootstrapping" | "online"
    },
    ["cloud.ep.tun.ip"] = {
      type    = "string",
      default = "",
    },
    ["cloud.ep.tun.port"] = {
      type    = "integer",
      default = 0,
    },
    ["cloud.ep.lwm2m.registered"] = {
      type    = "boolean",
      default = false,
    },
    ["cloud.ep.last.seen.unix"] = {
      type    = "integer",
      default = 0,
      min     = 0,
    },

    -- VPN subnet configuration.
    ["cloud.vpn.subnet"] = {
      type    = "string",
      default = "10.9.0.0/24",
    },

    -- Next available proxy port (bump-counter — allocate +1,
    -- write back).
    ["cloud.vpn.port.next"] = {
      type    = "integer",
      default = 5001,
      min     = 5001,
      max     = 6000,
    },
  },
}
