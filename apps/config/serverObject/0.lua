-- LwM2M Object 1 (Server), Instance 0.
--
-- Loaded by apps/src/main.cpp::load_provisioning_from_config to
-- synthesise the DM-server AccountProvisioning row pinned to the
-- Short Server ID at rid=0. Reader uses rids 0 (SSID), 1 (lifetime),
-- 7 (binding); the other rids are advisory and ignored by the
-- current loader.
--
-- Schema: see apps/config/deviceObject/0.lua header.

return {
  serverObject = {
    instance = 0,
    resources = {
      [0] = { description = "Short Server ID",                            value = 1,    include = true  },
      [1] = { description = "Lifetime",                                   value = 1500, include = true  },
      [2] = { description = "Default Minimum Period",                     value = 0,    include = false },
      [3] = { description = "Default Maximum Period",                     value = 0,    include = false },
      [4] = { description = "Disable",                                    value = 0,    include = false },
      [5] = { description = "Disable Timeout",                            value = 0,    include = false },
      [6] = { description = "Notification Storing When Disabled or Offline", value = 0, include = false },
      [7] = { description = "Binding",                                    value = "U",  include = true  },
      [8] = { description = "Registration Update Trigger",                value = 0,    include = false },
    },
  },
}
