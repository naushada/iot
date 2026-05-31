-- Example schema published by the iot client. Drop this file (or
-- any sibling *.lua) under the ds-server's `ds-schema-dir=` path
-- and ds-server will validate every set/get against it.
--
-- Each schema entry's `keys` table is keyed by the FULL key name
-- (the optional `namespace` field is informational). When two
-- files claim the same key, last-loaded wins — ds-server logs a
-- WARNING so silent ownership collisions are visible.

return {
  namespace = "iot",
  keys = {
    ["iot.lifetime"]   = {
      type    = "integer",
      default = 86400,
      min     = 0,
    },
    ["iot.endpoint"]   = {
      type    = "string",
      default = "urn:dev:client-1",
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
