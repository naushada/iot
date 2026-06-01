#include "ctrl.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include <ace/INET_Addr.h>      // for time-based send/recv (ACE_Time_Value)
#include <ace/LSOCK_Dgram.h>
#include <ace/Log_Msg.h>
#include <ace/Time_Value.h>
#include <ace/UNIX_Addr.h>

namespace wifi_client::ctrl {

// ───────────────────────── Parser ─────────────────────────────────

namespace {

/// Strip leading "<N>" priority prefix (wpa_ctrl prepends one) and
/// trailing CR/LF. Returns the trimmed slice.
std::string_view strip_prefix_and_eol(std::string_view line) {
    // Drop a leading "<digit>" sequence.
    if (line.size() >= 3 && line[0] == '<') {
        auto close = line.find('>');
        if (close != std::string_view::npos && close <= 4) {
            line.remove_prefix(close + 1);
        }
    }
    // Drop trailing CR / LF.
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.remove_suffix(1);
    }
    return line;
}

/// Pull the value of `key=...` from `tail`. Value is bounded by
/// the next whitespace OR end-of-string. Returns empty if the key
/// isn't present.
std::string find_kv(std::string_view tail, std::string_view key) {
    std::string needle{key};
    needle.push_back('=');
    auto pos = tail.find(needle);
    if (pos == std::string_view::npos) return {};
    pos += needle.size();
    // Stop at whitespace OR a closing bracket — wpa_supplicant
    // CONNECTED events wrap "id_str=<v>" inside square brackets,
    // so `]` is a value terminator even without whitespace.
    auto end = tail.find_first_of(" \t]", pos);
    if (end == std::string_view::npos) end = tail.size();
    return std::string(tail.substr(pos, end - pos));
}

/// Heuristic: pull "Connection to <bssid> completed" → bssid.
/// The CONNECTED event has the bssid right after "to ".
std::string find_connected_bssid(std::string_view tail) {
    constexpr std::string_view marker = "Connection to ";
    auto pos = tail.find(marker);
    if (pos == std::string_view::npos) return {};
    pos += marker.size();
    auto end = tail.find(' ', pos);
    if (end == std::string_view::npos) end = tail.size();
    return std::string(tail.substr(pos, end - pos));
}

/// Map a kind-prefix to the enum. Returns Unknown if no match.
CtrlEvent::Kind classify_prefix(std::string_view stripped) {
    constexpr std::pair<std::string_view, CtrlEvent::Kind> table[] = {
        {"CTRL-EVENT-SCAN-STARTED",  CtrlEvent::Kind::ScanStarted},
        {"CTRL-EVENT-SCAN-RESULTS",  CtrlEvent::Kind::ScanResults},
        {"CTRL-EVENT-CONNECTED",     CtrlEvent::Kind::Connected},
        {"CTRL-EVENT-DISCONNECTED",  CtrlEvent::Kind::Disconnected},
        {"CTRL-EVENT-ASSOC-REJECT",  CtrlEvent::Kind::AssocReject},
        {"CTRL-EVENT-AUTH-REJECT",   CtrlEvent::Kind::AuthReject},
        {"CTRL-EVENT-TERMINATING",   CtrlEvent::Kind::Terminating},
    };
    for (const auto& [needle, kind] : table) {
        if (stripped.size() >= needle.size()
            && stripped.compare(0, needle.size(), needle) == 0) {
            return kind;
        }
    }
    return CtrlEvent::Kind::Unknown;
}

} // namespace

CtrlEvent Parser::classify(std::string_view line) {
    auto stripped = strip_prefix_and_eol(line);
    CtrlEvent ev;
    ev.raw  = std::string(stripped);
    ev.kind = classify_prefix(stripped);

    switch (ev.kind) {
    case CtrlEvent::Kind::Connected:
        ev.bssid = find_connected_bssid(stripped);
        ev.ssid  = find_kv(stripped, "id_str");
        break;
    case CtrlEvent::Kind::Disconnected:
        ev.bssid  = find_kv(stripped, "bssid");
        ev.reason = find_kv(stripped, "reason");
        break;
    case CtrlEvent::Kind::AssocReject:
        ev.bssid  = find_kv(stripped, "bssid");
        ev.reason = find_kv(stripped, "status_code");
        break;
    case CtrlEvent::Kind::AuthReject:
        ev.bssid  = find_kv(stripped, "bssid");
        ev.reason = find_kv(stripped, "status_code");
        break;
    default:
        break;
    }
    return ev;
}

// ───────────────────────── Client ─────────────────────────────────

struct Client::Impl {
    ACE_LSOCK_Dgram sock;
    ACE_UNIX_Addr   peer;
    std::string     local_path;
    bool            open = false;
};

Client::Client() : m_impl(new Impl{}) {}

Client::~Client() {
    close();
    delete m_impl;
}

bool Client::connected() const { return m_impl && m_impl->open; }

bool Client::connect(const std::string& server_sock_path) {
    close();

    // Generate a unique local path: /tmp/wpa_ctrl_<pid>_<rand>.
    // Same idiom wpa_cli uses; wpa_supplicant routes replies via
    // recvfrom's source address, so the local bind must be a real
    // filesystem path it can write back to.
    char tmpl[64];
    std::snprintf(tmpl, sizeof(tmpl),
                  "/tmp/wpa_ctrl_iot_%d_XXXXXX",
                  static_cast<int>(::getpid()));
    int fd = ::mkstemp(tmpl);
    if (fd < 0) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [wifi:%t] %M %N:%l mkstemp(%C) errno=%d\n"),
                   tmpl, errno));
        return false;
    }
    // mkstemp creates the file; we need a unix-socket name to bind
    // to, not a regular file. Unlink it before bind so the bind can
    // create the socket node at the same path.
    ::close(fd);
    ::unlink(tmpl);
    m_impl->local_path = tmpl;

    ACE_UNIX_Addr local_addr(m_impl->local_path.c_str());
    if (m_impl->sock.open(local_addr) != 0) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [wifi:%t] %M %N:%l Dgram open(%C) errno=%d\n"),
                   m_impl->local_path.c_str(), errno));
        m_impl->local_path.clear();
        return false;
    }

    m_impl->peer.set(server_sock_path.c_str());
    m_impl->open = true;

    // Send ATTACH so wpa_supplicant starts forwarding events.
    std::string reply;
    if (!request("ATTACH", reply) || reply.compare(0, 2, "OK") != 0) {
        ACE_ERROR((LM_WARNING,
                   ACE_TEXT("%D [wifi:%t] %M %N:%l ATTACH failed; reply='%C'\n"),
                   reply.c_str()));
        close();
        return false;
    }
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [wifi:%t] %M %N:%l ctrl attached to %C\n"),
               server_sock_path.c_str()));
    return true;
}

bool Client::request(const std::string& cmd, std::string& reply) {
    if (!connected()) return false;

    std::string framed = cmd;
    if (framed.empty() || framed.back() != '\n') framed.push_back('\n');

    ssize_t n = m_impl->sock.send(framed.data(), framed.size(),
                                  m_impl->peer);
    if (n != static_cast<ssize_t>(framed.size())) {
        ACE_ERROR((LM_WARNING,
                   ACE_TEXT("%D [wifi:%t] %M %N:%l send('%C') n=%d errno=%d\n"),
                   cmd.c_str(), static_cast<int>(n), errno));
        return false;
    }

    char buf[4096];
    ACE_UNIX_Addr src;
    ACE_Time_Value timeout(2, 0);
    ssize_t got = m_impl->sock.recv(buf, sizeof(buf) - 1, src, 0, &timeout);
    if (got <= 0) {
        ACE_ERROR((LM_WARNING,
                   ACE_TEXT("%D [wifi:%t] %M %N:%l recv('%C') n=%d errno=%d\n"),
                   cmd.c_str(), static_cast<int>(got), errno));
        return false;
    }
    buf[got] = '\0';
    reply.assign(buf, got);
    while (!reply.empty() && (reply.back() == '\n' || reply.back() == '\r')) {
        reply.pop_back();
    }
    return reply.compare(0, 4, "FAIL") != 0;
}

std::optional<CtrlEvent> Client::recv_event(int timeout_ms) {
    if (!connected()) return std::nullopt;
    char buf[4096];
    ACE_UNIX_Addr src;
    ACE_Time_Value timeout(timeout_ms / 1000, (timeout_ms % 1000) * 1000);
    ssize_t got = m_impl->sock.recv(buf, sizeof(buf) - 1, src, 0, &timeout);
    if (got <= 0) return std::nullopt;
    buf[got] = '\0';
    return Parser::classify(std::string_view(buf, static_cast<std::size_t>(got)));
}

void Client::close() {
    if (!m_impl) return;
    if (m_impl->open) {
        m_impl->sock.close();
        m_impl->open = false;
    }
    if (!m_impl->local_path.empty()) {
        ::unlink(m_impl->local_path.c_str());
        m_impl->local_path.clear();
    }
}

} // namespace wifi_client::ctrl
