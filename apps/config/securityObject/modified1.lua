-- LwM2M Object 0 (Security), Instance 1 — scratch DM-server account
-- with opaque identity + secret-key bytes. Mirrors the existing
-- apps/config/securityObject/modified1.json which is not loaded by
-- the active code path (load_provisioning_from_config only consumes
-- 0.json / 1.json). Kept in-tree as a sample of the opaque-value
-- shape:
--
--   value = { bytes = { 107, 77, ... }, subtype = 16 }
--
-- bytes is the raw byte sequence (decimal); subtype = 16 means
-- application/octet-stream in the underlying nlohmann::json binary
-- subtype field. Lua callers can read this back as a table; the
-- proposed loader collapses { bytes, subtype } back into a binary
-- string before handing it to the C++ provisioning code.
--
-- Schema: see apps/config/deviceObject/0.lua header.

return {
  securityObject = {
    instance = 1,
    resources = {
      [0]  = { description = "LwM2M Server URI",                value = "coaps://0.0.0.0:5683", include = true  },
      [1]  = { description = "Bootstrap-Server",                value = false,                  include = true  },
      [2]  = { description = "Security Mode",                   value = 0,                      include = true  },
      [3]  = { description = "Public Key or Identity",
               value       = { bytes = { 107, 77, 168, 238, 6, 143, 75, 239, 22, 5, 97, 215, 84, 205, 177, 49 }, subtype = 16 },
               include     = true },
      [4]  = { description = "Server Public Key",               value = 0,                      include = true  },
      [5]  = { description = "Secret Key",
               value       = { bytes = { 247, 211, 198, 45, 134, 173, 3, 201, 163, 91, 2, 39, 34, 30, 131, 119 }, subtype = 16 },
               include     = true },
      [6]  = { description = "SMS Security Mode",               value = 0,                      include = false },
      [7]  = { description = "SMS Binding Key Parameters",      value = 0,                      include = false },
      [8]  = { description = "SMS Binding Secret Key(s)",       value = 0,                      include = false },
      [9]  = { description = "LwM2M Server SMS Number",         value = 0,                      include = false },
      [10] = { description = "Short Server ID",                 value = 1,                      include = false },
      [11] = { description = "Client Hold Off Time",            value = 0,                      include = false },
      [12] = { description = "Bootstrap-Server Account Timeout", value = 0,                     include = false },
    },
  },
}
