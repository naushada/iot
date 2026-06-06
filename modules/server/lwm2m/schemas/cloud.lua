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

    -- Bootstrap Server (L21/D3).  iot-cloudd reads these keys to
    -- configure the CoAP /bs endpoint and to build the Security +
    -- Server object TLV payloads that are pushed to devices during
    -- bootstrap.
    ["cloud.bs.uri"] = {
        access  = "Admin",
      type    = "string",
      default = "coaps://0.0.0.0:5684",
    },
    -- Security mode for bootstrapping devices.  "PSK" (default) uses a
    -- pre-shared key; "None" skips DTLS handshake (dev only).
    ["cloud.bs.security.mode"] = {
        access  = "Admin",
      type    = "string",
      default = "PSK",
    },
    -- PSK identity written into the device Security Object at bootstrap.
    ["cloud.bs.psk.id"] = {
        access  = "Admin",
      type    = "string",
      default = "iot-client",
    },
    -- PSK secret (opaque — hex or raw).  Written into the device
    -- Security Object RID 5 at bootstrap.
    ["cloud.bs.psk.key"] = {
        access  = "Admin",
      type    = "opaque",
      default = "",
    },
    -- Device Management server URI.  Written into the device Server
    -- Object (OID 1, RID 0) at bootstrap so the device knows where to
    -- register after bootstrapping.
    ["cloud.dm.uri"] = {
        access  = "Admin",
      type    = "string",
      default = "coaps://0.0.0.0:5683",
    },
    -- Default registration lifetime pushed to devices at bootstrap
    -- (Server Object OID 1, RID 1).  Devices may request a different
    -- value; the DM server can accept or negotiate.
    ["cloud.dm.lifetime"] = {
        access  = "Admin",
      type    = "integer",
      default = 86400,
      min     = 0,
      max     = 2592000,
    },
    -- Default binding mode pushed to devices at bootstrap
    -- (Server Object OID 1, RID 7).  "U" = UDP (standard).
    ["cloud.dm.binding"] = {
        access  = "Admin",
      type    = "string",
      default = "U",
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

    -- VPN PKI paths.  CA + server certs are generated at image build
    -- time (per-build CA, xpmile pattern).  ca.key is NOT in the image
    -- — mount at runtime via secret volume.
    ["cloud.vpn.ca.crt"] = {
      type    = "string",
      default = "/etc/iot/vpn/ca/ca.crt",
    },
    ["cloud.vpn.ca.key"] = {
      type    = "string",
      default = "/run/secrets/iot-ca-key/ca.key",
    },
    ["cloud.vpn.server.crt"] = {
      type    = "string",
      default = "/etc/iot/vpn/server.crt",
    },
    ["cloud.vpn.server.key"] = {
      type    = "string",
      default = "/etc/iot/vpn/server.key",
    },
  },
}
