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
-- ds-server is special: it cannot self-disable (the very socket
-- carrying the command would go dead). services.ds.enable is
-- intentionally OMITTED from this schema — ds-server's
-- SchemaRegistry namespace-claimed-but-key-not-declared path
-- rejects any set against it. The runtime state surface
-- (services.ds.state, services.ds.uptime.sec) is still published
-- so `ds-cli svc list` has a uniform shape (ENABLE column shows
-- "n/a" for ds).
--
-- Install at /etc/iot/ds-schemas/services.lua (ds-server
-- auto-loads on boot).

return {
  namespace = "services",
  keys = {
    -- ds-server: state surface only
    ["services.ds.state"]                  = { type = "string",  default = "running" },
    ["services.ds.uptime.sec"]             = { type = "integer", default = 0, min = 0 },

    -- net-router (leaf: no dependencies)
    ["services.net.router.enable"]         = { type = "boolean", default = true,
                                               depends_on = {} },
    ["services.net.router.state"]          = { type = "string",  default = "running" },

    -- openvpn-client (depends on net.router for forwarding)
    ["services.openvpn.client.enable"]     = { type = "boolean", default = true,
                                               depends_on = {"net.router"} },
    ["services.openvpn.client.state"]      = { type = "string",  default = "running" },

    -- lwm2m-client / lwm2m-server (depend on net.router for forwarding)
    ["services.lwm2m.client.enable"]       = { type = "boolean", default = true,
                                               depends_on = {"net.router"} },
    ["services.lwm2m.client.state"]        = { type = "string",  default = "running" },
    ["services.lwm2m.server.enable"]       = { type = "boolean", default = true,
                                               depends_on = {"net.router"} },
    ["services.lwm2m.server.state"]        = { type = "string",  default = "running" },

    -- wifi-client (depends on net.router for DHCP forwarding)
    ["services.wifi.client.enable"]        = { type = "boolean", default = true,
                                               depends_on = {"net.router"} },
    ["services.wifi.client.state"]         = { type = "string",  default = "running" },
  },
}
