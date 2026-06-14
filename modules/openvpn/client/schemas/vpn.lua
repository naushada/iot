-- vpn.* schema for openvpn-client (L12).
--
-- Read keys (operator configures via ds-cli):
--   The remote endpoint (host/port/proto) has NO schema default — it is the
--   cloud's to define. vpn.remote.host is *derived on the device* from the
--   bootstrap-delivered DM URI (the VPN concentrator is co-located with the DM
--   on the cloud VM), so a co-located cloud needs no VPN-host config here. The
--   cloud's LwM2M Object 2048 endpoint push still overrides all three — and is
--   the source for port/proto (which the cloud knows authoritatively; baking
--   defaults would risk dialing the wrong port/proto). The client stays gated
--   until host+port+proto are present (derived host + pushed port/proto).
--   vpn.remote.host   - server hostname or IP (device-derived from DM URI; cloud-override)
--   vpn.remote.port   - server port            (required; cloud-pushed, 1..65535)
--   vpn.remote.proto  - "tcp-client" or "udp"  (required; cloud-pushed)
--   vpn.cert.path     - client X.509 cert      (default /etc/iot/vpn/client.crt)
--   vpn.key.path      - client X.509 priv key  (default /etc/iot/vpn/client.key)
--   vpn.ca.path       - server CA              (default /etc/iot/vpn/ca.crt)
--   vpn.cipher        - data-channel cipher    (default "AES-256-GCM")
--   vpn.dev           - "tun" or "tap"         (default "tun")
--   vpn.mgmt.port     - mgmt iface port        (default 7505, 1024..65535)
--
-- Write keys (openvpn-client publishes back):
--   vpn.state             - "disconnected" / "resolving" / "connecting" /
--                           "auth" / "wait" / "connected" / "exited"
--   vpn.assigned.ip       - pushed virtual IP
--   vpn.assigned.gateway  - pushed VPN gateway
--   vpn.assigned.netmask  - pushed netmask
--   vpn.assigned.dns      - comma-joined DNS list (array variant: FUP)
--   vpn.pid               - live openvpn subprocess pid
--   vpn.exit_code         - last openvpn exit code (set when state=exited)
--   vpn.gate.reason       - "ok" while running, "wan_down" while gated on
--                           net.iface.active. Lets operators distinguish
--                           "VPN off because no WAN" from "off because exited".
--   vpn.bound.iface       - WAN iface this session is bound to (mirrors
--                           net.iface.active at spawn). Empty when idle.
--
-- Drop this file alongside iot.lua at ds-server's `ds-schema-dir=`
-- path (defaults to /etc/iot/ds-schemas/). ds-server auto-loads it
-- on boot; missing dir is silently treated as no-schemas.
--
-- Note: paths (cert/key/ca) are STRING-validated here. Filesystem
-- existence is checked by the daemon at start time, not at set time
-- (the schema can't stat() the host filesystem).

return {
  namespace = "vpn",
  keys = {
    -- ───────── Read keys (operator → daemon) ─────────
    -- Derived on the device from the bootstrap-delivered DM URI host (the VPN
    -- concentrator is co-located with the DM); the cloud's Object-2048 endpoint
    -- push overrides it for a split topology. No schema default either way.
    ["vpn.remote.host"]  = {
        access  = "Admin", type = "string"  },
    ["vpn.remote.port"]  = {
        access  = "Admin", type = "integer", min = 1, max = 65535 },
    ["vpn.remote.proto"] = {
        access  = "Admin", type = "string"  },

    -- Default to where the lwm2m-client materialises the cloud-pushed cert
    -- family (LwM2M Object 2048). This makes the openvpn client work on a
    -- bare-metal/systemd RPi with no seed step — the docker stack's vpn-config
    -- one-shot sets the same paths, so it stays a no-op override there.
    ["vpn.cert.path"]    = {
        access  = "Admin", type = "string", default = "/etc/iot/vpn/client.crt" },
    ["vpn.key.path"]     = {
        access  = "Admin", type = "string", default = "/etc/iot/vpn/client.key" },
    ["vpn.ca.path"]      = {
        access  = "Admin", type = "string", default = "/etc/iot/vpn/ca.crt" },

    ["vpn.cipher"]       = {
        access  = "Admin", type = "string",  default = "AES-256-GCM" },
    ["vpn.dev"]          = {
        access  = "Admin", type = "string",  default = "tun" },
    ["vpn.mgmt.port"]    = {
        access  = "Admin", type = "integer", default = 7505,
                             min = 1024, max = 65535 },

    -- ───────── Write keys (daemon → operator) ─────────
    ["vpn.state"]            = {
        access  = "Viewer", type = "string" },
    ["vpn.assigned.ip"]      = {
        access  = "Viewer", type = "string" },
    ["vpn.assigned.gateway"] = {
        access  = "Viewer", type = "string" },
    ["vpn.assigned.netmask"] = {
        access  = "Viewer", type = "string" },
    ["vpn.assigned.dns"]     = {
        access  = "Viewer", type = "string" },
    ["vpn.pid"]              = {
        access  = "Viewer", type = "integer", min = 0 },
    ["vpn.exit_code"]        = {
        access  = "Viewer", type = "integer" },
    ["vpn.gate.reason"]      = {
        access  = "Viewer", type = "string" },
    ["vpn.bound.iface"]      = {
        access  = "Viewer", type = "string" },
  },
}
