#ifndef __http_server_auth_hpp__
#define __http_server_auth_hpp__

/// Session-based authentication for iot-httpd (L19/D1).
///
/// Server-side session store with opaque token cookies. A SessionStore
/// is shared across all handlers (owned by main). The AuthGuard helper
/// wraps a Router::HandlerFn and returns 401 when the request carries
/// no valid session cookie.
///
/// Auth can be disabled at runtime by setting http.auth.enabled=false
/// in the data store — every route becomes public. The default is
/// enabled (auth required for all /api/v1/* routes except /api/v1/auth).
///
/// Credentials are read from the data store at startup and reloaded
/// every ~30s. The admin user is:
///   auth.users.admin.password  — plain-text password (v1; bcrypt FUP)
/// When unset, the compiled-in default is "admin".
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
        std::string role;          // "admin" (v1 — single role)
        std::chrono::steady_clock::time_point expires_at;
    };

    SessionStore() = default;

    /// Create a session for `username`, return the opaque token.
    /// The caller sets the token as a Set-Cookie header.
    std::string create_session(const std::string& username,
                               const std::string& role = "admin");

    /// Validate a token. Returns nullptr when expired or invalid.
    const Session* validate(const std::string& token);

    /// Destroy a session (logout).
    void destroy(const std::string& token);

    /// Sweep expired sessions. Call periodically (~every 60s).
    void sweep_expired();

    /// Whether auth is currently enabled (mirrors http.auth.enabled).
    bool enabled() const { return m_enabled.load(std::memory_order_acquire); }
    void set_enabled(bool v) { m_enabled.store(v, std::memory_order_release); }

private:
    std::mutex m_mutex;
    std::map<std::string, Session> m_sessions;   // token → session
    std::atomic<bool> m_enabled{true};

    /// Random token (URL-safe base64, 32 bytes of entropy).
    static std::string make_token();
};

// ── Auth helpers ───────────────────────────────────────────────────

/// Credential store backed by data-store keys.  Reloadable.
class CredentialStore {
public:
    /// Load the admin password from the data store.
    /// Key: auth.users.admin.password  — plain-text (v1)
    /// Falls back to compiled-in default "admin" when the key is unset.
    static std::string load_admin_password(data_store::Client& ds);

    /// Default admin password (used when the ds key is unset).
    static constexpr const char* kDefaultPassword = "admin";
};

/// Extract the session token from a Cookie header.
/// Returns empty string when no session cookie is present.
std::string extract_session_cookie(
    const std::map<std::string, std::string>& headers);

/// Build a Set-Cookie header value for the session token.
/// path=/; HttpOnly; SameSite=Strict. Max-Age defaults to 8h.
std::string make_set_cookie(const std::string& token,
                             int max_age_sec = 28800);

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
