-- LwM2M Object 5 (Firmware Update), Instance 0.
--
-- Loaded by apps/src/lwm2m_object_stubs.cpp::install_firmware to back
-- the CLI alias `firmware read=<rid>` (or generic `read path=/5/0/<rid>`).
-- All resources are read-only stubs today; wiring a real package-pull
-- + apply hook (rids 0 / 1 / 2) is a follow-up PR per RDD D6 ("raise
-- a Could object to a real backing only when a deployment asks").
--
-- Resources per OMA OMNA registry (subset implemented):
--   0 Package                opaque  (W; not implemented)
--   1 Package URI            string  (W; not implemented)
--   2 Update                 none    (E; not implemented)
--   3 State                  integer (R)  0=Idle 1=Downloading 2=Downloaded 3=Updating
--   5 Update Result          integer (R)  0=Initial
--   6 PkgName                string  (R)
--   7 PkgVersion             string  (R)
--   8 Firmware Update Protocol Support  integer (R; e.g. 0=CoAP, 1=CoAPS)
--   9 Firmware Update Delivery Method   integer (R)  0=Pull 1=Push 2=Both
--
-- Schema: see apps/config/deviceObject/0.lua header.

return {
  firmwareObject = {
    instance = 0,
    resources = {
      [3] = { description = "State",                            value = 0,         include = true },
      [5] = { description = "Update Result",                    value = 0,         include = true },
      [6] = { description = "Package Name",                     value = "iot",     include = true },
      [7] = { description = "Package Version",                  value = "0.1.0",   include = true },
      [8] = { description = "Firmware Update Protocol Support", value = 1,         include = true },   -- CoAPS
      [9] = { description = "Firmware Update Delivery Method",  value = 2,         include = true },   -- Pull + Push
    },
  },
}
