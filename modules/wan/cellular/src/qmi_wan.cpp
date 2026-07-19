#include "qmi_wan.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace cellular {

namespace {

/// Drop leading/trailing spaces and tabs.
std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r')) --b;
    return s.substr(a, b - a);
}

std::string strip_quotes(std::string s) {
    s = trim(s);
    if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

/// First line whose text contains `label`, with everything up to and including
/// the label's trailing ':' removed and the remainder trimmed. Returns "" when
/// no line matches. qmicli right-aligns its labels with variable leading
/// whitespace ("     IPv4 address: X", "IPv4 gateway address: Y"), so we match
/// on the label substring, not a fixed column.
std::string field_after(const std::string& out, const std::string& label) {
    std::istringstream ss(out);
    std::string line;
    while (std::getline(ss, line)) {
        const auto lp = line.find(label);
        if (lp == std::string::npos) continue;
        const auto cp = line.find(':', lp + label.size());
        if (cp == std::string::npos) continue;
        return trim(line.substr(cp + 1));
    }
    return {};
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

} // namespace

int mask_to_prefix(const std::string& mask) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    // sscanf keeps this dependency-free and rejects malformed input via the count.
    if (std::sscanf(mask.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 0;
    const unsigned long m = (a << 24) | (b << 16) | (c << 8) | d;
    // Count contiguous high bits; reject non-contiguous masks (not a valid CIDR).
    int prefix = 0;
    bool seen_zero = false;
    for (int i = 31; i >= 0; --i) {
        if (m & (1UL << i)) {
            if (seen_zero) return 0;   // 1 after a 0 → not a contiguous netmask
            ++prefix;
        } else {
            seen_zero = true;
        }
    }
    return prefix;
}

DirectIpSettings parse_current_settings(const std::string& out) {
    DirectIpSettings s;
    s.ip          = field_after(out, "IPv4 address");
    s.gateway     = field_after(out, "IPv4 gateway address");
    s.subnet_mask = field_after(out, "IPv4 subnet mask");
    if (!s.subnet_mask.empty()) s.prefix = mask_to_prefix(s.subnet_mask);

    const auto dns1 = field_after(out, "IPv4 primary DNS");
    const auto dns2 = field_after(out, "IPv4 secondary DNS");
    if (!dns1.empty()) s.dns.push_back(dns1);
    if (!dns2.empty()) s.dns.push_back(dns2);

    const auto mtu = field_after(out, "MTU");
    if (!mtu.empty()) s.mtu = std::atoi(mtu.c_str());

    s.valid = !s.ip.empty();
    return s;
}

StartResult parse_start_network(const std::string& out) {
    StartResult r;
    if (contains(out, "Network started")) {
        r.status = StartResult::Status::Started;
        // "	Packet data handle: '2235671520'"
        r.handle = strip_quotes(field_after(out, "Packet data handle"));
        return r;
    }
    // Order matters: a CID-alloc timeout is a distinct recovery path (kill the
    // wedged qmi-proxy and retry) vs a network-side CallFailed (wait + retry).
    if (contains(out, "CID allocation failed")) {
        r.status = StartResult::Status::CidTimeout;
        return r;
    }
    if (contains(out, "couldn't start network")) {
        r.status     = StartResult::Status::CallFailed;
        r.end_reason = field_after(out, "call end reason");
        r.verbose    = field_after(out, "verbose call end reason");
        return r;
    }
    if (contains(out, "error:")) {
        r.status = StartResult::Status::OtherError;
        return r;
    }
    r.status = StartResult::Status::Unknown;
    return r;
}

bool parse_ps_attached(const std::string& serving_system_out) {
    return strip_quotes(field_after(serving_system_out, "PS")) == "attached";
}

bool parse_connected(const std::string& packet_service_status_out) {
    return strip_quotes(field_after(packet_service_status_out, "Connection status"))
           == "connected";
}

} // namespace cellular
