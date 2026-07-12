-- smsctl.* schema for iot-smsctld — authenticated device control over SMS.
--
-- The daemon consumes the MT-SMS envelope cellular-client publishes
-- (sms.version + sms.last.*), authenticates the sender against the device's own
-- users (auth.users.admin.password.hash / auth.users.accounts — the same hashes
-- the device-ui login uses), executes one of a fixed command allowlist, and
-- answers with a single MO SMS via sms.send.*.
--
-- Command grammar (see apps/docs/tdd-smsctl.md):
--   IOT LOGIN <user> <password>     IOT LOGOUT          IOT STATUS
--   IOT REBOOT                      IOT FACTORY-RESET [<nonce>]
--   IOT APN <apn>                   IOT RADIO RESTART   IOT WIFI <ssid> [<psk>]
--
-- SHIPS DISABLED (smsctl.enabled=false). Enable per device, because the login
-- password necessarily crosses the carrier in plaintext — prefer a dedicated
-- Admin account for SMS over the shared `admin` one.
--
-- Read keys (operator -> daemon):
--   smsctl.enabled           - master switch; the daemon ignores all inbound
--                              SMS while false (default false)
--   smsctl.allowed.numbers   - CSV of E.164 senders permitted to issue
--                              commands. Empty = any sender may attempt LOGIN
--                              (the password is still required). A non-allowed
--                              sender is dropped SILENTLY - no reply, so the
--                              device is not an oracle and carrier spam costs
--                              nothing. Matching is on the last 9 digits, so
--                              "+919096383701" and "9096383701" are the same.
--   smsctl.session.ttl.sec   - login session lifetime (default 600)
--   smsctl.lockout.failures  - failed logins per sender before lockout (5)
--   smsctl.lockout.sec       - lockout window (default 900)
--
-- Write keys (daemon -> operator / device-ui):
--   smsctl.state       - "disabled" | "listening"
--   smsctl.last.sender - sender of the last command
--   smsctl.last.cmd    - the last command's KEYWORD ONLY. Arguments are never
--                        stored: a LOGIN password or a WiFi PSK must never
--                        reach ds or the journal.
--   smsctl.last.result - "ok" | "err"
--   smsctl.last.ts     - epoch seconds of the last command
--   smsctl.sessions    - number of live sessions
--   smsctl.version     - bump-on-change counter for the device-ui long-poll
--
-- Install at /etc/iot/ds-schemas/smsctl.lua (ds-server auto-loads).

local function viewer_str() return { access = "Viewer", type = "string", default = "" } end

return {
  namespace = "smsctl",
  keys = {
    -- ── config (operator-set) ──────────────────────────────────────────
    ["smsctl.enabled"]          = { access = "Admin", type = "boolean", default = false },
    ["smsctl.allowed.numbers"]  = { access = "Admin", type = "string",  default = "" },
    ["smsctl.session.ttl.sec"]  = { access = "Admin", type = "integer", default = 600,
                                    min = 60, max = 86400 },
    ["smsctl.lockout.failures"] = { access = "Admin", type = "integer", default = 5,
                                    min = 1,  max = 20 },
    ["smsctl.lockout.sec"]      = { access = "Admin", type = "integer", default = 900,
                                    min = 60, max = 86400 },

    -- ── status (daemon-published) ──────────────────────────────────────
    ["smsctl.state"]       = viewer_str(),
    ["smsctl.last.sender"] = viewer_str(),
    ["smsctl.last.cmd"]    = viewer_str(),   -- keyword only, NEVER arguments
    ["smsctl.last.result"] = viewer_str(),
    ["smsctl.last.ts"]     = viewer_str(),
    ["smsctl.sessions"]    = viewer_str(),
    ["smsctl.version"]     = viewer_str(),
  },
}
