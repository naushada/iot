#include "smsctl/session.hpp"

#include <algorithm>
#include <cstdio>

namespace smsctl {

namespace {

/// Trim + compare E.164-ish numbers leniently: an operator may store
/// "+919096383701" while the carrier delivers "919096383701" (or vice versa).
/// Compare on digits only, and match on the last 9 digits so a national vs
/// international prefix does not lock the operator out of their own device.
std::string digits_of(const std::string& s) {
    std::string d;
    for (char c : s) if (c >= '0' && c <= '9') d.push_back(c);
    return d;
}

bool same_number(const std::string& a, const std::string& b) {
    const std::string da = digits_of(a), db = digits_of(b);
    if (da.empty() || db.empty()) return false;
    const std::size_t n = std::min<std::size_t>(9, std::min(da.size(), db.size()));
    return da.compare(da.size() - n, n, db, db.size() - n, n) == 0;
}

} // namespace

bool SessionStore::sender_allowed(const std::string& sender) const {
    if (m_cfg.allowed_numbers.empty()) return true;   // allowlist off
    for (const auto& n : m_cfg.allowed_numbers)
        if (same_number(n, sender)) return true;
    return false;
}

std::string SessionStore::login(const std::string& sender,
                                const std::string& user,
                                const std::string& password,
                                const AccountLookup& lookup,
                                std::uint64_t now) {
    auto& f = m_failures[sender];
    if (f.locked_until > now) {
        const std::uint64_t mins = (f.locked_until - now + 59) / 60;
        return "locked out (" + std::to_string(mins) + " min)";
    }

    Account acct;
    const bool known = lookup && lookup(user, acct);
    // Same reply for "no such user" and "wrong password" — never confirm which
    // account names exist on the device.
    const bool ok = known && !acct.hash.empty() &&
                    !password.empty() && sha256_hex(password) == acct.hash;
    if (!ok) {
        if (++f.count >= m_cfg.lockout_failures) {
            f.locked_until = now + m_cfg.lockout_sec;
            f.count = 0;
            const std::uint64_t mins = (m_cfg.lockout_sec + 59) / 60;
            return "locked out (" + std::to_string(mins) + " min)";
        }
        return "invalid credentials";
    }

    f.count = 0;
    f.locked_until = 0;
    Session s;
    s.account    = acct;
    s.expires_at = now + m_cfg.session_ttl_sec;
    m_sessions[sender] = std::move(s);
    return {};                                        // success
}

void SessionStore::logout(const std::string& sender) {
    m_sessions.erase(sender);
    m_nonces.erase(sender);
}

const Account* SessionStore::session(const std::string& sender,
                                     std::uint64_t now) const {
    auto it = m_sessions.find(sender);
    if (it == m_sessions.end()) return nullptr;
    if (it->second.expires_at <= now) return nullptr;  // expired; sweep() reaps
    return &it->second.account;
}

void SessionStore::sweep(std::uint64_t now) {
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ) {
        if (it->second.expires_at <= now) it = m_sessions.erase(it);
        else                              ++it;
    }
    for (auto it = m_nonces.begin(); it != m_nonces.end(); ) {
        if (it->second.expires_at <= now) it = m_nonces.erase(it);
        else                              ++it;
    }
    for (auto it = m_failures.begin(); it != m_failures.end(); ) {
        // Drop the record once the lockout has elapsed and nothing is pending.
        if (it->second.count == 0 && it->second.locked_until <= now)
            it = m_failures.erase(it);
        else
            ++it;
    }
}

std::string SessionStore::mint_nonce(const std::string& sender,
                                     std::uint64_t now,
                                     std::uint64_t seed) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%06u",
                  static_cast<unsigned>(seed % 1000000ULL));
    Nonce n;
    n.value      = buf;
    n.expires_at = now + kNonceTtlSec;
    m_nonces[sender] = n;
    return n.value;
}

bool SessionStore::consume_nonce(const std::string& sender,
                                 const std::string& nonce,
                                 std::uint64_t now) {
    auto it = m_nonces.find(sender);
    if (it == m_nonces.end()) return false;
    const bool ok = it->second.expires_at > now && it->second.value == nonce;
    // Single use: any attempt burns it, so a wrong guess cannot be retried
    // against the same nonce.
    m_nonces.erase(it);
    return ok;
}

} // namespace smsctl
