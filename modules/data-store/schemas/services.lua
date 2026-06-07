-- services.* schema (L16/D1).
--
-- Operator-controlled enable/state plane for every iot daemon.
-- Each gateable daemon (X) owns:
--   services.X.enable   boolean default true   operator-flipped gate
--   services.X.state    string  default "stopped"
--                                  one of: stopped / running / disabled /
--                                  starting / stopping / exited / conflict
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

local keys = {
    -- ds-server: state surface only
    ["services.ds.state"]                  = {
        access  = "Viewer", type = "string",  default = "stopped" },
    ["services.ds.uptime.sec"]             = {
        access  = "Viewer", type = "integer", default = 0, min = 0 },

    -- net-router (leaf: no dependencies)
    ["services.net.router.enable"]         = {
        access  = "Admin", type = "boolean", default = true,
                                               depends_on = {},
                                               write_acl = {"uid:0"} },
    ["services.net.router.state"]          = {
        access  = "Viewer", type = "string",  default = "stopped" },

    -- openvpn-client (depends on net.router for forwarding)
    ["services.openvpn.client.enable"]     = {
        access  = "Admin", type = "boolean", default = true,
                                               depends_on = {"net.router"},
                                               write_acl = {"uid:0"} },
    ["services.openvpn.client.state"]      = {
        access  = "Viewer", type = "string",  default = "stopped" },

    -- lwm2m-client / lwm2m-server
    ["services.lwm2m.client.enable"]       = {
        access  = "Admin", type = "boolean", default = true,
                                               depends_on = {"net.router"},
                                               write_acl = {"uid:0"} },
    ["services.lwm2m.client.state"]        = {
        access  = "Viewer", type = "string",  default = "stopped" },
    ["services.lwm2m.server.enable"]       = {
        access  = "Admin", type = "boolean", default = true,
                                               depends_on = {"net.router"},
                                               write_acl = {"uid:0"} },
    ["services.lwm2m.server.state"]        = {
        access  = "Viewer", type = "string",  default = "stopped" },

    -- wifi-client
    ["services.wifi.client.enable"]        = {
        access  = "Admin", type = "boolean", default = true,
                                               depends_on = {"net.router"},
                                               write_acl = {"uid:0"} },
    ["services.wifi.client.state"]         = {
        access  = "Viewer", type = "string",  default = "stopped" },

    -- ── Cloud services (L21) ────────────────────────────────────
    ["services.cloud.iot.cloudd.enable"]   = {
        access  = "Admin", type = "boolean", default = true,
                                               depends_on = {"ds"},
                                               write_acl = {"uid:0"} },
    ["services.cloud.iot.cloudd.state"]    = {
        access  = "Viewer", type = "string",  default = "stopped" },
    ["services.cloud.iot.httpd.enable"]    = {
        access  = "Admin", type = "boolean", default = true,
                                               depends_on = {"ds"},
                                               write_acl = {"uid:0"} },
    ["services.cloud.iot.httpd.state"]     = {
        access  = "Viewer", type = "string",  default = "stopped" },
    ["services.cloud.openvpn.server.enable"] = {
        access  = "Admin", type = "boolean", default = true,
                                               depends_on = {"ds"},
                                               write_acl = {"uid:0"} },
    ["services.cloud.openvpn.server.state"]  = {
        access  = "Viewer", type = "string",  default = "stopped" },

    -- LwM2M CoAP server containers (lwm2m-bs / lwm2m-dm).
    -- These run the device lwm2m binary in role=server mode and are
    -- docker-compose managed (no enable toggle — always-on like ds).
    ["services.cloud.lwm2m.bs.state"]       = {
        access  = "Viewer", type = "string",  default = "stopped" },
    ["services.cloud.lwm2m.dm.state"]       = {
        access  = "Viewer", type = "string",  default = "stopped" },
}

-- ── L22 — per-daemon resource telemetry ──────────────────────────────
-- Four integer metrics per service, published every ~10s by each daemon's
-- StatsPublisher (cgroup v2/v1 for cpu/mem/threads + /proc/self/fd for the
-- descriptor count). Typed integers mirror services.ds.uptime.sec:
--   <svc>.cpu.permille  parts-per-1000 of one host-second (123 = 12.3 %)
--   <svc>.mem.rss.kb    resident memory, KB
--   <svc>.fd.count      open file descriptors
--   <svc>.threads       live task/thread count
-- One service per container, so these are effectively per-container totals.
local stats_services = {
  "services.ds",
  "services.net.router",
  "services.openvpn.client",
  "services.lwm2m.client",
  "services.lwm2m.server",
  "services.wifi.client",
  "services.cloud.iot.cloudd",
  "services.cloud.iot.httpd",
  "services.cloud.openvpn.server",
  "services.cloud.lwm2m.bs",
  "services.cloud.lwm2m.dm",
}
for _, s in ipairs(stats_services) do
  keys[s .. ".cpu.permille"] = { access = "Viewer", type = "integer", default = 0, min = 0 }
  keys[s .. ".cpu.count"]    = { access = "Viewer", type = "integer", default = 0, min = 0 }
  keys[s .. ".mem.rss.kb"]   = { access = "Viewer", type = "integer", default = 0, min = 0 }
  keys[s .. ".fd.count"]     = { access = "Viewer", type = "integer", default = 0, min = 0 }
  keys[s .. ".threads"]      = { access = "Viewer", type = "integer", default = 0, min = 0 }
end

-- Bumped by every daemon on each stats flush so the cloud UI long-polls a
-- single key instead of round-robining all <svc>.* metric keys (mirrors
-- log.version).
keys["services.stats.version"] = { access = "Viewer", type = "integer", default = 0 }

return {
  namespace = "services",
  keys = keys,
}
