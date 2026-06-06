#include "auth.hpp"

#include "data_store/client.hpp"
#include "data_store/value.hpp"

#include <openssl/rand.h>
#include <openssl/sha.h>

#include <ace/Log_Msg.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <sstream>
#include <string_view>

namespace http_server {

// ── Token helpers ──────────────────────────────────────────────────

namespace {

// URL-safe base64 alphabet (RFC 4648 §5).  No padding — we always emit
// exact-length tokens so the receiver can compute the byte length directly.
constexpr char kBase64Url[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// Encode `len` random bytes into a URL-safe base64 string.
std::string base64url_encode(const unsigned char* src, std::size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        std::uint32_t n = static_cast<std::uint32_t>(src[i]) << 16;
        if (i + 1 < len) n |= static_cast<std::uint32_t>(src[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<std::uint32_t>(src[i + 2]);
        out.push_back(kBase64Url[(n >> 18) & 0x3F]);
        out.push_back(kBase64Url[(n >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? kBase64Url[(n >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? kBase64Url[n & 0x3F] : '=');
    }
    // Strip padding — our tokens are always a multiple of 3 bytes so the
    // caller never sees padding, but be defensive.
    while (!out.empty() && out.back() == '=') out.pop_back();
    return out;
}

/// Parse "key=value; key2=value2" from a Cookie header.  Returns the
/// value of the cookie named `name`, or an empty string.
std::string cookie_value(const std::string& header, std::string_view name) {
    std::string_view h{header};
    while (!h.empty()) {
        // Skip leading whitespace / ';'
        auto semi = h.find(';');
        std::string_view part;
        if (semi == std::string_view::npos) {
            part = h;
            h = {};
        } else {
            part = h.substr(0, semi);
            h = h.substr(semi + 1);
        }
        // Trim leading space
        while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
        auto eq = part.find('=');
        if (eq == std::string_view::npos) continue;
        std::string_view key = part.substr(0, eq);
        std::string_view val = part.substr(eq + 1);
        if (key == name) return std::string(val);
    }
    return {};
}

} // namespace

// ── SessionStore ───────────────────────────────────────────────────

std::string SessionStore::make_token() {
    unsigned char buf[32];  // 256 bits of entropy
    if (RAND_bytes(buf, sizeof(buf)) != 1) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [http:%t] %M %N:%l RAND_bytes failed — "
                            "falling back to low-entropy token\n")));
        // Fallback: not cryptographically safe, but keeps the server running.
        for (auto& b : buf) b = static_cast<unsigned char>(std::rand() & 0xFF);
    }
    return "iot-" + base64url_encode(buf, sizeof(buf));
}

std::string SessionStore::create_session(const std::string& username,
                                          const std::string& role,
                                          const std::string& access) {
    std::string token = make_token();
    Session s;
    s.username   = username;
    s.role       = role;
    s.access     = access;
    s.expires_at = std::chrono::steady_clock::now() + std::chrono::hours(8);

    std::lock_guard<std::mutex> lk(m_mutex);
    m_sessions[token] = s;
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [http:%t] %M %N:%l session created for %C "
                        "(role=%C, access=%C, %zu active)\n"),
               username.c_str(), role.c_str(), access.c_str(),
               m_sessions.size()));
    return token;
}

const SessionStore::Session* SessionStore::validate(const std::string& token) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_sessions.find(token);
    if (it == m_sessions.end()) return nullptr;
    if (std::chrono::steady_clock::now() > it->second.expires_at) {
        m_sessions.erase(it);
        return nullptr;
    }
    return &it->second;
}

void SessionStore::destroy(const std::string& token) {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_sessions.erase(token);
}

void SessionStore::sweep_expired() {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto now = std::chrono::steady_clock::now();
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ) {
        if (now > it->second.expires_at) {
            it = m_sessions.erase(it);
        } else {
            ++it;
        }
    }
}

// ── SHA-256 ─────────────────────────────────────────────────────────

std::string sha256_hex(const std::string& input) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()),
           input.size(), digest);
    // Encode as lowercase hex
    constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(SHA256_DIGEST_LENGTH * 2);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        out.push_back(kHex[digest[i] >> 4]);
        out.push_back(kHex[digest[i] & 0xF]);
    }
    return out;
}

// ── CredentialStore ────────────────────────────────────────────────

std::string CredentialStore::load_admin_password_hash(
    data_store::Client& ds) {
    std::vector<data_store::Client::GetResult> got;
    auto rs = ds.get({"auth.users.admin.password.hash"}, got);
    if (rs.ok && !got.empty() && got[0].has_value) {
        if (auto s = data_store::to_string(got[0].value)) {
            if (!s->empty()) return *s;
        }
    }
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [http:%t] %M %N:%l auth.users.admin.password.hash "
                        "unset — using compiled-in default\n")));
    return kDefaultHash;
}

bool CredentialStore::verify(const std::string& submitted_plaintext,
                              const std::string& stored_hash) {
    return sha256_hex(submitted_plaintext) == stored_hash;
}

std::string CredentialStore::load_user_access(data_store::Client& ds,
                                               const std::string& username) {
    std::string key = "auth.users." + username + ".access";
    std::vector<data_store::Client::GetResult> got;
    auto rs = ds.get({key}, got);
    if (rs.ok && !got.empty() && got[0].has_value) {
        if (auto s = data_store::to_string(got[0].value)) {
            if (*s == "Viewer") return "Viewer";
        }
    }
    return "Admin";  // default: full access
}

// ── Access control ─────────────────────────────────────────────────

bool can_write(const std::string& access_level, const std::string& /*key*/) {
    // "Admin" can write anything; "Viewer" is read-only.
    // Future: per-key granularity via schema lookup.
    return access_level == "Admin";
}

// ── Cookie helpers ─────────────────────────────────────────────────

std::string extract_session_cookie(
    const std::map<std::string, std::string>& headers) {
    auto it = headers.find("cookie");
    if (it == headers.end()) return {};
    return cookie_value(it->second, "iot-session");
}

std::string make_set_cookie(const std::string& token, int max_age_sec) {
    std::ostringstream oss;
    oss << "iot-session=" << token
        << "; Path=/"
        << "; HttpOnly"
        << "; SameSite=Strict"
        << "; Max-Age=" << max_age_sec;
    return oss.str();
}

// ── Auth guard ─────────────────────────────────────────────────────

bool is_public_route(const std::string& path) {
    return path == "/api/v1/auth/login" || path == "/api/v1/auth/logout";
}

Router::HandlerFn with_auth(Router::HandlerFn next,
                              SessionStore& store) {
    return [next = std::move(next), &store](
               const HttpParser::Request& req) -> HttpResponse {
        // Auth-disabled → pass through
        if (!store.enabled()) return next(req);

        // Public routes → no auth
        if (is_public_route(req.path)) return next(req);

        // Validate session cookie
        std::string token = extract_session_cookie(req.headers);
        if (token.empty()) {
            HttpResponse r;
            r.status = 401;
            r.body   = R"({"ok":false,"err":"authentication required"})";
            return r;
        }
        const auto* session = store.validate(token);
        if (!session) {
            HttpResponse r;
            r.status = 401;
            r.body   = R"({"ok":false,"err":"session expired or invalid"})";
            return r;
        }
        return next(req);
    };
}

} // namespace http_server
