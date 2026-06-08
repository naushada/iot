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
    ["cloud.bs.endpoint"] = {
        access  = "Admin",
      type    = "string",
      default = "urn:dev:gateway-",
    },
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
    -- DM-level PSK for post-bootstrap device communication.
    -- Separate from cloud.bs.psk.* (used only during bootstrapping).
    -- Per-device overrides stored in cloud.provision.configs per endpoint.
    ["cloud.dm.psk.id"] = {
        access  = "Admin",
      type    = "string",
      default = "iot-dm-client",
    },
    ["cloud.dm.psk.key"] = {
        access  = "Admin",
      type    = "opaque",
      default = "",
    },
    -- LwM2M version reported by devices / expected by the DM server.
    -- Per-device overrides stored in cloud.provision.configs as
    -- lwm2m.version inside the endpoint's JSON entry.
    ["cloud.dm.lwm2m.version"] = {
        access  = "Admin",
      type    = "string",
      default = "1.1",
    },

    -- Provision request: iot-httpd writes the endpoint name here;
    -- iot-cloudd watches this key and calls BootstrapProvisioner.
    ["cloud.provision.request"] = {
        access  = "Admin",
      type    = "string",
      default = "",
    },
    -- Per-endpoint Security Object (OID 0) configs — a table of
    -- tables keyed by endpoint name, stored as a JSON string:
    -- {
    --   "urn:dev:gateway-42": {
    --     "sec.uri":      "coaps://cloud:5683",
    --     "sec.mode":     0,
    --     "sec.identity": "iot-client",
    --     "sec.key":      "0102...",
    --     "sec.bs":       1,
    --     "sec.ssid":     0
    --   }
    -- }
    -- iot-cloudd reads the matching entry at provision time.
    ["cloud.provision.configs"] = {
        access  = "Admin",
      type    = "string",
      default = "{}",
    },
    -- Deprovision request: iot-httpd writes the endpoint name here;
    -- iot-cloudd watches this key and calls deprovision().
    ["cloud.deprovision.request"] = {
        access  = "Admin",
      type    = "string",
      default = "",
    },

    -- ── PSK provisioning (serial-derived endpoint + per-endpoint PSK) ──
    -- See apps/docs/tdd-psk-provisioning.md.
    --
    -- Per-endpoint credential array. The BS/DM servers load this live
    -- (watch + add_credential) instead of the shared env-var PSK. The
    -- engineer pastes (serial, BS PSK) from device-ui; the backend forms
    -- the identity rpi<serial>@cloud.local and mints the DM PSK. JSON:
    --   [ { "serial":"<raw>", "identity":"rpi<raw>@cloud.local",
    --       "bs.psk.key":"<hex>", "dm.psk.id":"rpi<raw>@cloud.local",
    --       "dm.psk.key":"<hex>" }, ... ]
    -- Write-only / no ds-cli read (mirrors the device rule): only the
    -- cloud-svc server processes may read; reveal via cloud.dev.mode.
    ["cloud.endpoint.credentials"] = {
        access    = "Admin",
        type      = "string",
        default   = "[]",
        write_acl = {"gid:cloud-svc"},
        read_acl  = {"gid:cloud-svc"},
    },
    -- Provision carrier: the engineer pastes the device-generated BS PSK
    -- here, then sets cloud.provision.request = serial (the trigger).
    -- iot-cloudd reads this, mints the DM PSK, upserts the credential
    -- array, then clears this back to "". Write-only (cloud-svc) — the
    -- cloud-httpd write succeeds under cloud.dev.mode (ACL bypass).
    ["cloud.provision.bs.psk"] = {
        access    = "Admin",
        type      = "string",
        default   = "",
        write_acl = {"gid:cloud-svc"},
        read_acl  = {"gid:cloud-svc"},
    },
    -- Commissioning flag: while true the ds-server bypasses the PSK ACLs
    -- so an operator can inspect/edit credentials on the cloud.
    ["cloud.dev.mode"] = {
        access    = "Admin",
        type      = "boolean",
        default   = true,   -- simple dev commissioning out of the box; set false to lock down
        write_acl = {"gid:cloud-svc"},
    },

    -- VPN subnet for tunnel IP allocation.
    ["cloud.vpn.subnet"] = {
      type    = "string",
      default = "10.9.0.0/24",
    },

    -- OpenVPN server listen socket (iot-cloudd spawns openvpn(8) here).
    ["cloud.vpn.listen.port"] = {
      type    = "integer",
      default = 1194,
      min     = 1,
      max     = 65535,
    },
    ["cloud.vpn.proto"] = {
      type    = "string",
      default = "tcp-server",   -- "tcp-server" | "udp"
    },
    ["cloud.vpn.cipher"] = {
      type    = "string",
      default = "AES-256-GCM",
    },
    ["cloud.vpn.dev"] = {
      type    = "string",
      default = "tun",          -- "tun" | "tap"
    },
    ["cloud.vpn.mgmt.port"] = {
      type    = "integer",
      default = 7506,
      min     = 1,
      max     = 65535,
    },
    ["cloud.vpn.verb"] = {
      type    = "integer",
      default = 3,
      min     = 0,
      max     = 11,
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
