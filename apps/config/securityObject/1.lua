-- LwM2M Object 0 (Security), Instance 1 — DM-server account.
--
-- isBootstrapServer (rid=1) = false marks this instance as the
-- Device-Management account paired with serverObject/0.lua via the
-- Short Server ID at rid=10.
--
-- Schema: see apps/config/deviceObject/0.lua header.

return {
  securityObject = {
    instance = 1,
    resources = {
      [0]  = { description = "LwM2M Server URI",                value = "coaps://0.0.0.0:5683", include = true  },
      [1]  = { description = "Bootstrap-Server",                value = false,                  include = true  },
      [2]  = { description = "Security Mode",                   value = 0,                      include = true  },
      [3]  = { description = "Public Key or Identity",          value = 0,                      include = true  },
      [4]  = { description = "Server Public Key",               value = 0,                      include = true  },
      [5]  = { description = "Secret Key",                      value = 0,                      include = true  },
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
