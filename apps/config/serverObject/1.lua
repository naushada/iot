-- LwM2M Object 1 (Server), Instance 1.
--
-- The active loader (main.cpp::load_provisioning_from_config) only
-- reads serverObject/0.lua; this file is reserved for future
-- multi-server deployments and is intentionally empty.
--
-- Schema: see apps/config/deviceObject/0.lua header.

return {
  serverObject = {
    instance  = 1,
    resources = {},
  },
}
