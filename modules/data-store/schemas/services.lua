-- services.* schema (L16/D1).
--
-- Operator-controlled enable/state plane for every iot daemon.
-- Each gateable daemon (X) owns:
--   services.X.enable   boolean default true   operator-flipped gate
--   services.X.state    string  default "running"
--                                  one of: running / disabled / starting /
--                                  stopping / exited / conflict
--
-- L17a/D1 — each enable key declares an optional depends_on array
-- of bare service names (e.g. "net.router"). When a dependency is
-- disabled, dependents set gate.reason="dep_down:<name>".
--
-- L17c — write_acl restricts which peers can set a key. An empty
-- or absent ACL means unrestricted. v1 policy: services.*.enable
-- keys are root-only; state keys are daemon-writable (unrestricted
-- since daemons typically run as root anyway).
--
-- ds-server is special: it cannot self-disable (the very socket
-- carrying the command would go dead). services.ds.enable is
-- intentionally OMITTED from this schema.
--
-- Install at /etc/iot/ds-schemas/services.lua (ds-server
-- auto-loads on boot).

return {
  namespace = "services",
  keys = {
    -- ds-server: state surface only
    ["services.ds.state"]                  = {
        access  = "Viewer", type = "string",  default = "running" },
    ["services.ds.uptime.sec"]             = {
        access  = "Viewer", type = "integer", default = 0, min = 0 },

    -- net-router (leaf: no dependencies)
    ["services.net.router.enable"]         = {
        access  = "Admin", type = "boolean", default = true,
                                               depends_on = {},
                                               write_acl = {"uid:0"} },
    ["services.net.router.state"]          = {
        access  = "Viewer", type = "string",  default = "running" },

    -- openvpn-client (depends on net.router for forwarding)
    ["services.openvpn.client.enable"]     = {
        access  = "Admin", type = "boolean", default = true,
                                               depends_on = {"net.router"},
                                               write_acl = {"uid:0"} },
    ["services.openvpn.client.state"]      = {
        access  = "Viewer", type = "string",  default = "running" },

    -- lwm2m-client / lwm2m-server
    ["services.lwm2m.client.enable"]       = {
        access  = "Admin", type = "boolean", default = true,
                                               depends_on = {"net.router"},
                                               write_acl = {"uid:0"} },
    ["services.lwm2m.client.state"]        = {
        access  = "Viewer", type = "string",  default = "running" },
    ["services.lwm2m.server.enable"]       = {
        access  = "Admin", type = "boolean", default = true,
                                               depends_on = {"net.router"},
                                               write_acl = {"uid:0"} },
    ["services.lwm2m.server.state"]        = {
        access  = "Viewer", type = "string",  default = "running" },

    -- wifi-client
    ["services.wifi.client.enable"]        = {
        access  = "Admin", type = "boolean", default = true,
                                               depends_on = {"net.router"},
                                               write_acl = {"uid:0"} },
    ["services.wifi.client.state"]         = {
        access  = "Viewer", type = "string",  default = "running" },
  },
}
