#ifndef __http_server_auth_hpp__
#define __http_server_auth_hpp__

/// Session-based authentication for iot-httpd (L19/D1).
///
/// Server-side session store with opaque token cookies. A SessionStore
/// is shared across all handlers (owned by main).
///
/// Auth can be disabled at runtime by setting http.auth.enabled=false
/// in the data store — every route becomes public. The default is
/// enabled (auth required for all /api/v1/* routes except /api/v1/auth).
///
/// Credentials — SHA-256 (no plain text):
///   Login body:   { "id": "admin", "password": "<plaintext>" }
///   Stored key:   auth.users.admin.password.hash  (64-char hex digest)
///   Comparison:   sha256(submitted_plaintext) == stored_hash
///   Default hash: sha256("admin")
///
/// Thread safety: SessionStore is protected by an internal mutex.
/// Token generation uses OpenSSL RAND_bytes (already linked for TLS).

#include "router.hpp"

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>

namespace data_store { class Client; }

namespace http_server {

// ── SessionStore ───────────────────────────────────────────────────

class SessionStore {
public:
    struct Session {
        std::string username;
        std::string role;          // "admin"
        std::string access;        // "Admin" or "Viewer"
        std::chrono::steady_clock::time_point expires_at;
    };

    SessionStore() = default;

    /// Create a session for `username`, return the opaque token.
    /// The caller sets the token as a Set-Cookie header.
    std::string create_session(const std::string& username,
                               const std::string& role = "admin",
                               const std::string& access = "Admin");

    /// Validate a token. Returns nullptr when expired or invalid.
    const Session* validate(const std::string& token);

    /// Destroy a session (logout).
    void destroy(const std::string& token);

    /// Sweep expired sessions. Call periodically (~every 60s).
    void sweep_expired();

    /// Whether auth is currently enabled (mirrors http.auth.enabled).
    bool enabled() const { return m_enabled.load(std::memory_order_acquire); }
    void set_enabled(bool v) { m_enabled.store(v, std::memory_order_release); }

    /// Session cookie name (mirrors http.auth.cookie.name). Must differ
    /// between same-host/different-port instances (cloud vs proxied device
    /// UI) so their cookies don't clobber each other. Thread-safe.
    std::string cookie_name() const;
    void set_cookie_name(const std::string& name);

private:
    mutable std::mutex m_mutex;
    std::map<std::string, Session> m_sessions;   // token → session
    std::atomic<bool> m_enabled{true};
    std::string m_cookie_name{"iot-session"};    // guarded by m_mutex

    /// Random token (URL-safe base64, 32 bytes of entropy).
    static std::string make_token();
};

// ── Auth helpers ───────────────────────────────────────────────────

/// Compute the SHA-256 digest of `input` and return it as 64 lowercase
/// hex characters.  Uses OpenSSL (already linked for TLS).
std::string sha256_hex(const std::string& input);

/// Credential store backed by data-store keys.  Reloadable.
class CredentialStore {
public:
    /// Load the admin password hash from the data store.
    /// Key: auth.users.admin.password.hash  — SHA-256 hex digest
    /// Falls back to the compiled-in default (sha256("admin")) when
    /// the key is unset.
    static std::string load_admin_password_hash(data_store::Client& ds);

    /// Verify a plain-text password against the stored SHA-256 hash.
    /// Returns true when sha256(submitted) == stored_hash.
    static bool verify(const std::string& submitted_plaintext,
                       const std::string& stored_hash);

    /// Load the user's access level from the data store.
    /// Key: auth.users.<username>.access — "Admin" or "Viewer"
    /// Falls back to "Admin" when unset.
    static std::string load_user_access(data_store::Client& ds,
                                         const std::string& username);

    /// Default admin password hash: sha256("admin").
    static constexpr const char* kDefaultHash =
        "8c6976e5b5410415bde908bd4dee15df"
        "b167a9c873fc4bb8a81f6f2ab448a918";
};

/// Check whether `access_level` permits modifying `key`.
/// "Admin" can modify anything; "Viewer" is read-only.
bool can_write(const std::string& access_level, const std::string& key);

/// Extract the session token from a Cookie header, looking up the cookie
/// named `cookie_name`. Returns empty string when not present.
std::string extract_session_cookie(
    const std::map<std::string, std::string>& headers,
    const std::string& cookie_name = "iot-session");

/// Build a Set-Cookie header value for the session token under `cookie_name`.
/// path=/; HttpOnly; SameSite=Strict. Max-Age defaults to 8h.
std::string make_set_cookie(const std::string& token,
                             const std::string& cookie_name = "iot-session",
                             int max_age_sec = 28800);

/// Build a Set-Cookie header value that clears the session cookie (Max-Age=0).
std::string make_clear_cookie(const std::string& cookie_name = "iot-session");

/// Wrap a HandlerFn with an auth check.  When the request carries a
/// valid session cookie the wrapped handler is called normally;
/// otherwise a 401 JSON response is returned.  When `store` has
/// enabled()==false the check is a no-op (all requests pass through).
Router::HandlerFn with_auth(Router::HandlerFn next,
                              SessionStore& store);

/// Convenience: check whether `path` is a public route (no auth required).
/// Currently: /api/v1/auth/login and /api/v1/auth/logout are public.
bool is_public_route(const std::string& path);

} // namespace http_server

#endif
