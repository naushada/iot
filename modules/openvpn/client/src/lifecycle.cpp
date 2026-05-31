#include "lifecycle.hpp"

#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace openvpn_client {

namespace {

/// Lowercase a string in place (ascii only — vpn.state values are
/// ascii-pure).
std::string tolower_copy(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// Split `s` on whitespace into tokens. Used for ifconfig +
/// dhcp-option payloads.
std::vector<std::string> ws_split(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string tok;
    while (iss >> tok) out.push_back(std::move(tok));
    return out;
}

/// Join a vector with `,` (used to assemble multiple DNS push lines
/// into the vpn.assigned.dns comma-joined value).
std::string join_csv(const std::vector<std::string>& v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) out.push_back(',');
        out += v[i];
    }
    return out;
}

} // namespace

std::string Lifecycle::normalise_state(const std::string& s) {
    // OpenVPN STATE codes (per management-notes.txt): CONNECTING,
    // WAIT, AUTH, GET_CONFIG, ASSIGN_IP, ADD_ROUTES, CONNECTED,
    // RECONNECTING, EXITING, RESOLVE, TCP_CONNECT. Map the ones the
    // schema cares about; pass-through the rest (lowercased) so the
    // operator still sees something useful.
    if (s == "CONNECTING" || s == "TCP_CONNECT") return "connecting";
    if (s == "RESOLVE")                          return "resolving";
    if (s == "WAIT")                             return "wait";
    if (s == "AUTH")                             return "auth";
    if (s == "GET_CONFIG")                       return "wait";
    if (s == "ASSIGN_IP" || s == "ADD_ROUTES")   return "connecting";
    if (s == "CONNECTED")                        return "connected";
    if (s == "EXITING")                          return "exited";
    return tolower_copy(s);
}

void Lifecycle::step(const mgmt::Event& ev) {
    using K = mgmt::Event::Kind;

    if (ev.kind == K::State && ev.fields.size() >= 4) {
        // fields[1] = STATE name; fields[3] = local IP (set when
        // the state is ASSIGN_IP / CONNECTED).
        const auto& code = ev.fields[1];
        const auto& assigned = ev.fields[3];
        if (m_sinks.set_state) m_sinks.set_state(normalise_state(code));
        if (!assigned.empty() && m_sinks.set_assigned_ip) {
            m_sinks.set_assigned_ip(assigned);
        }
        return;
    }

    if (ev.kind == K::PushReply) {
        // Each field is "name [value...]"; dispatch each known one.
        std::vector<std::string> dns;
        for (const auto& opt : ev.fields) {
            auto [name, val] = mgmt::split_push_option(opt);
            if (name == "ifconfig") {
                // "10.8.0.6 255.255.255.0" → ip + netmask
                auto toks = ws_split(val);
                if (toks.size() >= 1 && m_sinks.set_assigned_ip) {
                    m_sinks.set_assigned_ip(toks[0]);
                }
                if (toks.size() >= 2 && m_sinks.set_assigned_netmask) {
                    m_sinks.set_assigned_netmask(toks[1]);
                }
            } else if (name == "route-gateway") {
                if (m_sinks.set_assigned_gateway) {
                    m_sinks.set_assigned_gateway(val);
                }
            } else if (name == "dhcp-option") {
                // "DNS 1.1.1.1" — strip the "DNS " prefix, collect.
                auto toks = ws_split(val);
                if (toks.size() >= 2 && toks[0] == "DNS") {
                    dns.push_back(toks[1]);
                }
            }
        }
        if (!dns.empty() && m_sinks.set_assigned_dns) {
            m_sinks.set_assigned_dns(join_csv(dns));
        }
        if (!m_saw_push_reply) {
            m_saw_push_reply = true;
            if (m_sinks.on_first_push_reply) m_sinks.on_first_push_reply();
        }
        return;
    }

    // Other event kinds (Banner, Hold, Log, ByteCount, Fatal, ...)
    // are observed but not acted on by v1. The reactor wrapper logs
    // them via ACE_DEBUG.
}

} // namespace openvpn_client
