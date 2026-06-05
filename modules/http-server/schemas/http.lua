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

    -- Handler thread-pool size. 0 = run handlers inline on the reactor
    -- thread (default). >0 off-loads handlers to N worker threads so a
    -- blocking long-poll can't stall other connections. CLI: http-workers=N.
    ["http.workers"]       = {
        access  = "Admin", type = "integer", default = 0, min = 0, max = 64 },

  },
}
