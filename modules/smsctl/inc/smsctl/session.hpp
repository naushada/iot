#ifndef __smsctl_session_hpp__
#define __smsctl_session_hpp__

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

/**
 * @file session.hpp
 * @brief Per-sender login sessions, brute-force lockout and the
 *        factory-reset confirmation nonce.
 *
 * Pure and host-testable: the clock is injected as an epoch-seconds argument,
 * so tests fast-forward time without sleeping. Sessions live only in memory —
 * a daemon restart drops them, which is deliberate (no persisted auth state).
 *
 * SMS sender IDs are spoofable on some carrier routes, so the MSISDN binding
 * is a convenience, NOT the authentication: the password check is the gate.
 */

namespace smsctl {

/// Lowercase-hex SHA-256. Must match iot-httpd's CredentialStore hashing so
/// SMS login accepts the very same passwords as the device-ui login.
std::string sha256_hex(const std::string& input);

/// A user record as resolved from ds (`auth.users.admin.password.hash` or an
/// entry of the `auth.users.accounts` JSON array).
struct Account {
    std::string id;
    std::string hash;              ///< sha256 hex of the password
    std::string access = "Viewer"; ///< "Admin" | "Viewer"
};

/// Resolve an account id → record. Returns false when the user is unknown.
/// Injected so the engine stays free of ds.
using AccountLookup = std::function<bool(const std::string& id, Account& out)>;

struct Config {
    std::uint32_t session_ttl_sec   = 600;
    std::uint32_t lockout_failures  = 5;
    std::uint32_t lockout_sec       = 900;
    /// E.164 numbers permitted to issue commands. Empty = any sender may try
    /// to log in (the password is still required).
    std::vector<std::string> allowed_numbers;
};

class SessionStore {
public:
    explicit SessionStore(Config cfg = {}) : m_cfg(std::move(cfg)) {}

    void set_config(Config cfg) { m_cfg = std::move(cfg); }
    const Config& config() const { return m_cfg; }

    /// True when `sender` may issue commands at all (allowlist gate). A
    /// non-allowed sender is dropped SILENTLY by the caller — no reply, so the
    /// device is not an oracle and we never burn credit on spam.
    bool sender_allowed(const std::string& sender) const;

    /// Verify credentials and open a session bound to `sender`.
    /// Returns an empty string on success, else a reply-safe reason
    /// ("invalid credentials" / "locked out (N min)"). Never echoes the
    /// password. A successful login clears the failure counter.
    std::string login(const std::string& sender, const std::string& user,
                      const std::string& password, const AccountLookup& lookup,
                      std::uint64_t now);

    void logout(const std::string& sender);

    /// The live session's account, or nullptr when absent/expired.
    const Account* session(const std::string& sender, std::uint64_t now) const;

    /// Drop expired sessions + nonces (called from the daemon's 1s timer).
    void sweep(std::uint64_t now);

    std::size_t session_count() const { return m_sessions.size(); }

    // ── factory-reset confirmation nonce ────────────────────────────────
    /// Mint a 6-digit single-use nonce for `sender`, valid `kNonceTtlSec`.
    /// Deterministic input (`seed`) keeps this pure/testable; the daemon feeds
    /// it entropy.
    std::string mint_nonce(const std::string& sender, std::uint64_t now,
                           std::uint64_t seed);
    /// Consume the nonce. True only for an exact, unexpired, unused match —
    /// any outcome invalidates it (single use, no guessing loop).
    bool consume_nonce(const std::string& sender, const std::string& nonce,
                       std::uint64_t now);

    static constexpr std::uint64_t kNonceTtlSec = 300;

private:
    struct Session {
        Account       account;
        std::uint64_t expires_at = 0;
    };
    struct Failures {
        std::uint32_t count = 0;
        std::uint64_t locked_until = 0;
    };
    struct Nonce {
        std::string   value;
        std::uint64_t expires_at = 0;
    };

    Config                            m_cfg;
    std::map<std::string, Session>    m_sessions;   // sender → session
    std::map<std::string, Failures>   m_failures;   // sender → brute-force state
    std::map<std::string, Nonce>      m_nonces;     // sender → pending nonce
};

} // namespace smsctl

#endif /* __smsctl_session_hpp__ */
