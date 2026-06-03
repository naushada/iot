-- http.* schema (L18/D2).
--
-- Configuration surface for the iot-httpd REST API server.
-- Values are read at startup; hot-reload is FUP.
--
-- Install at /etc/iot/ds-schemas/http.lua (ds-server auto-loads
-- on boot; iot-httpd reads them at startup via data_store::Client).

return {
  namespace = "http",
  keys = {
    -- Listening address. "0.0.0.0" binds all interfaces;
    -- "127.0.0.1" restricts to localhost.
    ["http.listen.ip"]     = { type = "string",  default = "0.0.0.0" },

    -- Listening port. 1–65535. Default 8080 avoids privileged <1024.
    ["http.listen.port"]   = { type = "integer", default = 8080,
                               min = 1, max = 65535 },

    -- Scheme. "http" only in v1 (TLS terminated by reverse proxy).
    -- Reserved for future "https" support.
    ["http.listen.scheme"] = { type = "string",  default = "http" },
  },
}
