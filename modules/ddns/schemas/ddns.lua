-- ddns.* schema for the iot-ddnsd device-side Dynamic DNS daemon.
--
-- iot-ddnsd keeps a public DNS A record pointed at the device's current public
-- IPv4 across four provider backends (dyndns2 / duckdns / cloudflare / route53).
-- The device cannot observe its own public IP (it sees only private/LAN
-- addresses), so the daemon discovers it via an HTTPS echo service and only
-- calls the provider when the IP changed or a forced-refresh window elapsed.
-- Ships DISABLED by default. See apps/docs/tdd-ddns.md + prd-ddns.md.
--
-- Read keys (operator → daemon), set via device-ui / ds-cli:
--   ddns.enabled          - master on/off (default false)
--   ddns.provider         - dyndns2 | duckdns | cloudflare | route53
--   ddns.hostname         - FQDN to keep updated (e.g. dev-<serial>.example.com)
--   ddns.interval         - detect/poll cadence, seconds (default 300)
--   ddns.refresh.force    - forced re-push window, seconds (default 86400); a
--                           re-push even when the IP is unchanged, to defend
--                           against out-of-band edits + free-tier expiry.
--   ddns.ip.source        - echo | dyndns2-auto | cloud (default echo)
--   ddns.<provider>.*     - per-provider targets (non-secret), see below.
--   ddns.token.path       - optional file holding the active provider's secret
--                           (mode 0640), used instead of the write-only key.
--
-- Secret keys (write-only): stored so the daemon can read them but ds-cli /
-- device-ui viewers cannot read them back. read_acl/write_acl gate them to the
-- daemon's runtime group. NOTE: the gid below (iot) is a placeholder pending
-- FR-9 (#521), which pins it to iot-ddnsd's final runtime user; keep it in sync
-- with the unit's User=/Group=.
--
-- Write keys (daemon → operator / device-ui). String-typed status the daemon
-- publishes verbatim; the device-ui renders them:
--   ddns.state       - disabled/waiting-clock/detecting/updating/ok/
--                      ok-unreachable/error
--   ddns.last.ip     - last public IPv4 we published
--   ddns.last.ok.ts  - epoch (seconds) of the last successful update
--   ddns.last.error  - last provider/echo error (human readable, no secrets)
--   ddns.version     - bump-on-change counter for the device-ui long-poll

local function viewer_str()  return { access = "Viewer", type = "string",  default = "" } end
local function admin_str(d)  return { access = "Admin",  type = "string",  default = d } end
-- Write-only secret: Admin base access, but read + write gated to the daemon's
-- group so ds-cli (root) and device-ui viewers cannot read the value back.
local function secret_str()
  return { access = "Admin", type = "string", default = "",
           write_acl = {"gid:iot"}, read_acl = {"gid:iot"} }
end

return {
  namespace = "ddns",
  keys = {
    -- ── config (operator-set) ─────────────────────────────────────────────
    ["ddns.enabled"]       = { access = "Admin", type = "boolean", default = false },
    ["ddns.provider"]      = admin_str("dyndns2"),
    ["ddns.hostname"]      = admin_str(""),
    ["ddns.interval"]      = { access = "Admin", type = "integer", default = 300,   min = 30, max = 86400 },
    ["ddns.refresh.force"] = { access = "Admin", type = "integer", default = 86400, min = 60, max = 604800 },
    ["ddns.ip.source"]     = admin_str("echo"),
    ["ddns.token.path"]    = admin_str(""),

    -- ── provider targets (non-secret) ─────────────────────────────────────
    ["ddns.dyndns2.server"]  = admin_str("members.dyndns.org"),
    ["ddns.dyndns2.user"]    = admin_str(""),
    ["ddns.duckdns.domains"] = admin_str(""),
    ["ddns.cf.zone.id"]      = admin_str(""),
    ["ddns.cf.record.name"]  = admin_str(""),
    ["ddns.r53.zone.id"]     = admin_str(""),
    ["ddns.r53.record.name"] = admin_str(""),
    ["ddns.r53.access.key"]  = admin_str(""),

    -- ── secrets (write-only) ──────────────────────────────────────────────
    ["ddns.dyndns2.token"] = secret_str(),
    ["ddns.duckdns.token"] = secret_str(),
    ["ddns.cf.token"]      = secret_str(),
    ["ddns.r53.secret.key"] = secret_str(),

    -- ── runtime state (daemon → device-ui) ────────────────────────────────
    ["ddns.state"]      = viewer_str(),
    ["ddns.last.ip"]    = viewer_str(),
    ["ddns.last.ok.ts"] = { access = "Viewer", type = "integer", default = 0 },
    ["ddns.last.error"] = viewer_str(),
    ["ddns.version"]    = { access = "Viewer", type = "integer", default = 0 },
  },
}
