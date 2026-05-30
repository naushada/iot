-- LwM2M Object 0 (Security), Instance 0 — bootstrap-server account.
--
-- Loaded by apps/src/main.cpp::load_provisioning_from_config into a
-- SecurityInstance. The active loader reads rids 0 (Server URI), 1
-- (Bootstrap-Server flag), 2 (Security Mode), 3 (Identity), 5 (Secret
-- Key), 10 (Short Server ID); other rids carry advisory defaults.
--
-- isBootstrapServer (rid=1) = true marks this instance as the BS
-- account; the matching DM account lives at instance 1.
--
-- Schema: see apps/config/deviceObject/0.lua header.

return {
  securityObject = {
    instance = 0,
    resources = {
      [0]  = { description = "LwM2M Server URI",                value = "coaps://0.0.0.0:5684", include = true  },
      [1]  = { description = "Bootstrap-Server",                value = true,                   include = true  },
      [2]  = { description = "Security Mode",                   value = 0,                      include = true  },
      [3]  = { description = "Public Key or Identity",          value = 0,                      include = true  },
      [4]  = { description = "Server Public Key",               value = 0,                      include = true  },
      [5]  = { description = "Secret Key",                      value = 0,                      include = true  },
      [6]  = { description = "SMS Security Mode",               value = 0,                      include = false },
      [7]  = { description = "SMS Binding Key Parameters",      value = 0,                      include = false },
      [8]  = { description = "SMS Binding Secret Key(s)",       value = 0,                      include = false },
      [9]  = { description = "LwM2M Server SMS Number",         value = 0,                      include = false },
      [10] = { description = "Short Server ID",                 value = 0,                      include = true  },
      [11] = { description = "Client Hold Off Time",            value = 0,                      include = false },
      [12] = { description = "Bootstrap-Server Account Timeout", value = 0,                     include = false },
    },
  },
}
