-- Schema published by the iot client. Drop this file (and any sibling
-- *.lua) under ds-server's `ds-schema-dir=` path — default
-- `/etc/iot/ds-schemas/` — and ds-server will validate every set/get
-- against it. Get on a missing key returns the schema default (when
-- declared); set is rejected with SchemaRejected (0x8004) on a type
-- mismatch or out-of-range integer.
--
-- Each schema entry's `keys` table is keyed by the FULL key name
-- (the optional `namespace` field is informational). When two
-- files claim the same key, last-loaded wins — ds-server logs a
-- WARNING so silent ownership collisions are visible.
--
-- Note: `iot.server.uri` has no `default` on purpose. The iot binary
-- treats absence as "fall back to the security-object .lua file";
-- declaring a default here would shadow that fallback for every run.

return {
  namespace = "iot",
  keys = {
    ["iot.lifetime"]   = {
      type    = "integer",
      default = 86400,
      min     = 0,
      max     = 2592000,     -- 30 days; LwM2M Registration lifetime cap
    },
    ["iot.endpoint"]   = {
      type    = "string",
      default = "urn:dev:client-1",
    },
    ["iot.server.uri"] = {
      type    = "string",
    },
    ["iot.binding"]    = {
      type    = "string",
      default = "U",
    },
    ["iot.identity"]   = {
      type    = "opaque",
    },
    ["iot.observable"] = {
      type    = "boolean",
      default = true,
    },
  },
}
