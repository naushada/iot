-- LwM2M Object 2 (Access Control), Instance 0.
--
-- Loaded by apps/src/lwm2m_object_stubs.cpp::install_access_control to
-- back the CLI alias `access-control read=<rid>` (or generic
-- `read path=/2/0/<rid>`). One default ACE entry granting full access
-- to the canonical DM-server Short Server ID (1).
--
-- Resources per OMA OMNA registry:
--   0 Object ID                    integer
--   1 Object Instance ID           integer
--   2 ACL                          multi-instance integer (compressed
--                                   here to a single-value placeholder;
--                                   full multi-instance support is a
--                                   follow-up)
--   3 Access Control Owner         integer (Short Server ID)
--
-- Schema: see apps/config/deviceObject/0.lua header.

return {
  accessControlObject = {
    instance = 0,
    resources = {
      [0] = { description = "Object ID",            value = 3, include = true },
      [1] = { description = "Object Instance ID",   value = 0, include = true },
      [2] = { description = "ACL",                  value = 15, include = true },   -- 0x0F = R+W+E+D
      [3] = { description = "Access Control Owner", value = 1, include = true },   -- DM SSID
    },
  },
}
