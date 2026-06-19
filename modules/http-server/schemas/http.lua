-- http.* schema (L18/D2).
--
-- Configuration surface for the iot-httpd REST API server.
-- http.listen.{ip,port,scheme} and http.tls.{cert,key,ca} are
-- hot-reloaded at runtime (FUP-L18-2): change them with ds-cli and
-- iot-httpd re-binds / rotates the cert within ~2s, no restart. Only
-- http.workers needs a restart.
--
-- Install at /etc/iot/ds-schemas/http.lua (ds-server auto-loads
-- on boot; iot-httpd reads them at startup via data_store::Client).

return {
  namespace = "http",
  keys = {
    -- Listening address. "0.0.0.0" binds all interfaces;
    -- "127.0.0.1" restricts to localhost.
    ["http.listen.ip"]     = {
        access  = "Admin", type = "string",  default = "0.0.0.0" },

    -- Listening port. 1–65535. Default 8080 avoids privileged <1024.
    ["http.listen.port"]   = {
        access  = "Admin", type = "integer", default = 8080,
                               min = 1, max = 65535 },

    -- Scheme. "http" = plain HTTP/1.1; "https" = native TLS termination
    -- (requires http.tls.cert + http.tls.key below). A reverse proxy in
    -- front can still terminate TLS instead — leave this "http" if so.
    ["http.listen.scheme"] = {
        access  = "Admin", type = "string",  default = "http" },

    -- TLS server certificate chain (PEM, leaf first). Required when
    -- scheme = "https". CLI override: http-cert=<path>.
    ["http.tls.cert"]      = {
        access  = "Admin", type = "string",  default = "" },

    -- TLS private key (PEM) matching http.tls.cert. Required when
    -- scheme = "https". CLI override: http-key=<path>. Keep mode 0600.
    ["http.tls.key"]       = {
        access  = "Admin", type = "string",  default = "" },

    -- Optional CA bundle (PEM). When set, mutual-TLS is enforced: clients
    -- must present a certificate that verifies against this CA. Empty =
    -- server-only TLS. CLI override: http-ca=<path>.
    ["http.tls.ca"]        = {
        access  = "Admin", type = "string",  default = "" },

    -- Handler thread-pool size. 0 = run handlers inline on the reactor thread;
    -- >0 off-loads handlers to N worker threads so a blocking long-poll (the
    -- shared /status poll, the Terminal shell) can't stall other connections.
    -- Default 4: both the device-ui and cloud-ui rely on those long-polls, so
    -- inline (0) stalls them — 4 is a safe floor with headroom for the status
    -- poll + a shell + a couple of requests. Operators tune it on the HTTP page;
    -- iot-httpd applies a change by self-restarting (the pool is sized at
    -- startup). CLI http-workers=N still overrides at startup if ever passed.
    ["http.workers"]       = {
        access  = "Admin", type = "integer", default = 4, min = 0, max = 64 },

    -- ── Remote shell (device-ui Terminal page) ──────────────────────
    -- Master switch for the forkpty-backed shell at /api/v1/shell/*.
    -- OFF by default: this is a remote shell on the device — runs as the
    -- iot-httpd service user (DynamicUser, NOT root), but still the
    -- single largest attack surface here, so an operator must opt in
    -- explicitly. Read on every /api/v1/shell/* request, so flipping it
    -- off kills new sessions immediately and existing ones on next poll.
    -- Always Admin-gated regardless of this flag. Each open terminal
    -- holds one blocking long-poll worker, so run http.workers >= 2 when
    -- enabling. NOTE: a retype (bool↔other) needs a schema bump on-device
    -- (opkg overwrites ds-schemas/*.lua); leave as bool.
    ["http.shell.enabled"] = {
        access  = "Admin", type = "boolean", default = false },

    -- Idle timeout (seconds): a shell session whose output is not polled
    -- for this long is reaped (SIGHUP'd + waited). Guards against orphaned
    -- shells when a browser tab is closed without a clean /close.
    ["http.shell.idle.sec"] = {
        access  = "Admin", type = "integer", default = 300,
                               min = 30, max = 3600 },

    -- Hard cap on concurrent shell sessions (each ties up one long-poll
    -- worker). Keeps a runaway client from exhausting the worker pool.
    ["http.shell.max.sessions"] = {
        access  = "Admin", type = "integer", default = 4, min = 1, max = 32 },

  },
}
