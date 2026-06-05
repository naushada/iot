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
        access  = "Admin",
      type    = "integer",
      default = 86400,
      min     = 0,
      max     = 2592000,     -- 30 days; LwM2M Registration lifetime cap
    },
    ["iot.endpoint"]   = {
        access  = "Admin",
      type    = "string",
      default = "urn:dev:client-1",
    },
    ["iot.server.uri"] = {
        access  = "Admin",
      type    = "string",
    },
    ["iot.binding"]    = {
        access  = "Admin",
      type    = "string",
      default = "U",
    },
    ["iot.identity"]   = {
        access  = "Admin",
      type    = "opaque",
    },
    ["iot.observable"] = {
        access  = "Admin",
      type    = "boolean",
      default = true,
    },
    -- Log level for all iot daemons.  Each daemon reads this key
    -- and adjusts its ACE_Log_Msg priority mask at startup and on
    -- change (hot-reload).  Valid values (case-insensitive):
    --   DEBUG, INFO, WARNING, ERROR
    -- Default: INFO (production-safe; no debug noise).
    ["log.level"] = {
        access  = "Admin",
      type    = "string",
      default = "INFO",
    },
    -- Recent log output from all iot daemons (plain text, newline-
    -- separated).  Each daemon appends timestamped lines.  Displayed
    -- in the UI as a scrollable textarea via GET /api/v1/log.
    -- Capped at ~200 lines / ~16 KB.
    ["log.text"] = {
        access  = "Viewer",
      type    = "string",
      default = "",
    },
  },
}
