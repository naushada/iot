#include "nft_rules.hpp"

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace net_router::nft {

namespace {

/// Whitespace-trim both ends.
std::string trim(std::string s) {
    auto issp = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && issp(s.back())) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && issp(s[i])) ++i;
    return s.substr(i);
}

} // namespace

std::vector<std::uint16_t> parse_forward_ports(const std::string& csv) {
    std::vector<std::uint16_t> out;
    std::string cur;
    auto flush = [&]() {
        std::string t = trim(cur);
        cur.clear();
        if (t.empty()) return;
        try {
            std::size_t pos = 0;
            unsigned long v = std::stoul(t, &pos);
            if (pos != t.size()) return;          // junk after digits
            if (v == 0 || v > 65535) return;       // out of u16 range
            out.push_back(static_cast<std::uint16_t>(v));
        } catch (...) {
            // Bad token — skip silently. Caller can log if needed.
        }
    };
    for (char c : csv) {
        if (c == ',') flush();
        else          cur.push_back(c);
    }
    flush();
    return out;
}

std::vector<CustomRule>
parse_custom_rules(const std::string& json, std::string* parse_error) {
    std::vector<CustomRule> out;
    if (json.empty()) return out;
    try {
        auto j = nlohmann::json::parse(json);
        if (!j.is_array()) {
            if (parse_error) *parse_error = "net.custom.rules: top-level must be a JSON array";
            return {};
        }
        for (const auto& e : j) {
            if (!e.is_object()) continue;
            CustomRule r;
            if (e.contains("action") && e["action"].is_string())  r.action = e["action"].get<std::string>();
            if (e.contains("proto")  && e["proto"].is_string())   r.proto  = e["proto"].get<std::string>();
            if (e.contains("dport")  && e["dport"].is_number_unsigned())  r.dport  = e["dport"].get<std::uint32_t>();
            if (e.contains("sport")  && e["sport"].is_number_unsigned())  r.sport  = e["sport"].get<std::uint32_t>();
            if (e.contains("to_ip")  && e["to_ip"].is_string())   r.to_ip  = e["to_ip"].get<std::string>();
            if (e.contains("to_port") && e["to_port"].is_number_unsigned()) r.to_port = e["to_port"].get<std::uint32_t>();
            // Minimal validation — action + proto required.
            if (r.action.empty() || r.proto.empty()) continue;
            out.push_back(std::move(r));
        }
    } catch (const std::exception& e) {
        if (parse_error) *parse_error = std::string("net.custom.rules: ") + e.what();
        return {};
    }
    return out;
}

std::string build_nft_ruleset(const State& s) {
    std::ostringstream ss;
    // Flush our scoped table first so re-apply is idempotent. We never
    // touch other writers' tables (NetworkManager, docker, fail2ban) —
    // each scopes by its own table name.
    ss << "flush table inet iot_router\n";
    ss << "table inet iot_router {\n";

    // ──────────── prerouting (NAT) ────────────
    // Inbound traffic from the tun interface gets DNAT'd to the
    // lwm2m client's target IP, one rule per forwarded port. If
    // no target IP is set OR no ports, emit an empty chain so
    // `nft -f -` still parses cleanly.
    ss << "    chain prerouting {\n";
    ss << "        type nat hook prerouting priority -100; policy accept;\n";
    if (!s.lwm2m_target_ip.empty() && !s.forward_ports.empty()) {
        for (auto p : s.forward_ports) {
            // Forward both TCP and UDP — covers HTTP/HTTPS (TCP) and
            // CoAP/DTLS (UDP) without the operator having to specify.
            ss << "        iifname \"" << s.tun_dev
               << "\" tcp dport " << p
               << " dnat to " << s.lwm2m_target_ip << ":" << p << "\n";
            ss << "        iifname \"" << s.tun_dev
               << "\" udp dport " << p
               << " dnat to " << s.lwm2m_target_ip << ":" << p << "\n";
        }
    }
    ss << "    }\n";

    // ──────────── forward (filter) ────────────
    // Default-accept + return early on tun→tun traffic. Custom rules
    // appended below.
    ss << "    chain forward {\n";
    ss << "        type filter hook forward priority 0; policy accept;\n";
    if (!s.tun_dev.empty()) {
        ss << "        iifname \"" << s.tun_dev
           << "\" oifname \"" << s.tun_dev << "\" return\n";
    }
    for (const auto& r : s.custom) {
        ss << "        ";
        // protocol clause
        ss << r.proto;
        if (r.dport) ss << " dport " << r.dport;
        if (r.sport) ss << " sport " << r.sport;
        // action clause
        if (r.action == "drop")        ss << " drop";
        else if (r.action == "accept") ss << " accept";
        else if (r.action == "forward" && !r.to_ip.empty()) {
            ss << " dnat to " << r.to_ip;
            if (r.to_port) ss << ":" << r.to_port;
        } else {
            // Unrecognised action — leave as a comment so operators
            // see it landed somewhere (rather than silently dropped).
            ss << " # unknown action '" << r.action << "'";
        }
        ss << "\n";
    }
    ss << "    }\n";

    ss << "}\n";
    return ss.str();
}

} // namespace net_router::nft
