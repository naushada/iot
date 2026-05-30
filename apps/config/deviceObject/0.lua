-- LwM2M Object 3 (Device), Instance 0.
--
-- Loaded by apps/src/lwm2m_object_3_device.cpp::load_overrides as
-- per-RID overrides for the canonical Device Object. Only entries
-- with include=true are applied; the rest fall through to the
-- compiled-in defaults (memory note: project_lwm2m_object3_backing.md).
--
-- Schema (every config file under apps/config/ follows the same shape):
--   return {
--     <object_name> = {
--       instance  = <iid>,
--       resources = { [<rid>] = { description = "...", value = ..., include = true|false }, ... },
--     },
--   }
--
-- Value types: string | integer | boolean | { bytes = {...}, subtype = N }
-- for opaque resources.

return {
  deviceObject = {
    instance = 0,
    resources = {
      [0]  = { description = "Manufacturer",                value = "Sierra Wireless",   include = true },
      [1]  = { description = "Model Number",                value = "LwM2M Client",      include = true },
      [2]  = { description = "Serial Number",               value = "A123456789ABCD",    include = true },
      [3]  = { description = "Firmware Version",            value = "0.1.0",             include = true },
      [14] = { description = "UTC Offset",                  value = "+00:00",            include = true },
      [15] = { description = "Timezone",                    value = "Etc/UTC",           include = true },
      [16] = { description = "Supported Binding and Modes", value = "U",                 include = true },
      [17] = { description = "Device Type",                 value = "LwM2M IoT Device",  include = true },
      [18] = { description = "Hardware Version",            value = "rev-A",             include = true },
      [19] = { description = "Software Version",            value = "0.1.0",             include = true },
    },
  },
}
