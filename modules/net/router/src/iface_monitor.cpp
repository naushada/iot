#include "iface_monitor.hpp"

#include <nlohmann/json.hpp>

namespace net_router::iface {

namespace {

using json = nlohmann::json;

void parse_link_show(const std::string& body, State& out) {
    if (body.empty()) return;
    try {
        auto j = json::parse(body);
        // `ip -j link show <name>` returns a one-element array.
        if (!j.is_array() || j.empty()) return;
        const auto& e = j[0];
        out.present = true;
        if (e.contains("operstate") && e["operstate"].is_string()) {
            out.up = (e["operstate"].get<std::string>() == "UP");
        }
        // Belt-and-braces: some kernels report operstate=UNKNOWN even
        // for working ifaces (typical for tun*); fall back to the
        // flags array containing both "UP" and "LOWER_UP".
        if (!out.up && e.contains("flags") && e["flags"].is_array()) {
            bool has_up = false, has_lower = false;
            for (const auto& f : e["flags"]) {
                if (!f.is_string()) continue;
                const auto s = f.get<std::string>();
                if (s == "UP")       has_up = true;
                if (s == "LOWER_UP") has_lower = true;
            }
            out.up = has_up && has_lower;
        }
    } catch (const std::exception&) {
        // Leave defaults; caller treats as not-usable.
    }
}

// True for an address that can carry WAN traffic: not loopback and not
// an IPv4 link-local (169.254.0.0/16, auto-assigned when DHCP fails —
// the very case where the iface is "up" but unusable).
bool is_routable_v4(const std::string& ip) {
    if (ip.rfind("127.", 0) == 0)     return false;
    if (ip.rfind("169.254.", 0) == 0) return false;
    return !ip.empty();
}

void parse_addr_show(const std::string& body, State& out) {
    if (body.empty()) return;
    try {
        auto j = json::parse(body);
        if (!j.is_array() || j.empty()) return;
        const auto& e = j[0];
        if (!e.contains("addr_info") || !e["addr_info"].is_array()) return;
        for (const auto& a : e["addr_info"]) {
            if (!a.contains("family") || a["family"] != "inet") continue;
            if (!a.contains("local") || !a["local"].is_string()) continue;
            if (is_routable_v4(a["local"].get<std::string>())) {
                out.addr    = true;
                out.addr_ip = a["local"].get<std::string>();
                return;
            }
        }
    } catch (const std::exception&) {
        // Leave defaults; caller treats as not-usable.
    }
}

void parse_route_default(const std::string& body, State& out) {
    if (body.empty()) return;
    try {
        auto j = json::parse(body);
        if (!j.is_array() || j.empty()) return;
        const auto& e = j[0];
        if (e.contains("gateway") && e["gateway"].is_string()) {
            out.gateway = e["gateway"].get<std::string>();
        }
    } catch (const std::exception&) {
        // No default route via this iface — fine, gateway stays empty.
    }
}

} // namespace

State probe(const std::string& name, shell::Runner runner) {
    State s;
    s.name = name;
    if (name.empty() || !runner) return s;

    int rc = 0;
    auto link = runner({"ip", "-j", "link", "show", name}, &rc);
    if (rc != 0) return s;          // iface absent
    parse_link_show(link, s);

    auto addr = runner({"ip", "-j", "addr", "show", name}, nullptr);
    parse_addr_show(addr, s);

    auto route = runner({"ip", "-j", "route", "show", "default", "dev", name},
                        nullptr);
    parse_route_default(route, s);
    return s;
}

std::vector<State> probe_all(const std::vector<std::string>& names,
                             shell::Runner runner) {
    std::vector<State> out;
    out.reserve(names.size());
    for (const auto& n : names) out.push_back(probe(n, runner));
    return out;
}

std::optional<std::size_t> pick_active(const std::vector<State>& states) {
    for (std::size_t i = 0; i < states.size(); ++i) {
        // A link that's OPER-UP but has no routable IPv4 (e.g. eth0 with
        // the cable in but no DHCP lease) can't carry WAN traffic — skip
        // it so net.iface.active reflects a genuinely usable path.
        if (states[i].present && states[i].up && states[i].addr) return i;
    }
    return std::nullopt;
}

} // namespace net_router::iface
