-- auth.* schema (L19/D1).
--
-- Authentication surface for the iot-httpd REST API. Credentials are
-- stored in the data store so the operator can change the admin
-- password at runtime via ds-cli without touching the filesystem.
--
-- Login request shape (POST /api/v1/auth/login):
--   { "id": "admin", "password": "<plaintext>" }
--
-- The server SHA-256-hashes the submitted password and compares it
-- against auth.users.admin.password_hash.  There is no plain-text path.
--
-- Install at /etc/iot/ds-schemas/auth.lua (ds-server auto-loads on
-- boot; iot-httpd reads them through the data_store::Client).

return {
  namespace = "auth",
  keys = {
    -- Admin password — SHA-256 hex digest (64 lowercase hex chars).
    -- Default value is the SHA-256 of "admin":
    --   echo -n "admin" | sha256sum
    --   8c6976e5b5410415bde908bd4dee15dfb167a9c873fc4bb8a81f6f2ab448a918
    ["auth.users.admin.password_hash"] = {
      type    = "string",
      default = "8c6976e5b5410415bde908bd4dee15dfb167a9c873fc4bb8a81f6f2ab448a918",
    },

    -- Admin user access level. "Admin" = full read/write.
    -- "Viewer" = read-only (cannot modify any configuration).
    ["auth.users.admin.access"] = {
      type    = "string",
      default = "Admin",
    },

    -- Enable / disable session-auth for the REST API.  When true
    -- (default), all /api/v1/* routes except /api/v1/auth/* require a
    -- valid session cookie.  Hot-reloaded ~every 60s.
    ["http.auth.enabled"] = {
      type    = "boolean",
      default = true,
    },
  },
}
