#include "container_net.hpp"

#include <cstdlib>
#include <sstream>
#include <vector>

namespace containers {

namespace {

// Parse "a.b.c.d" into 4 octets; false on malformed / out-of-range.
bool parse_ipv4(const std::string& s, int oct[4]) {
    std::stringstream ss(s);
    std::string part;
    int i = 0;
    while (std::getline(ss, part, '.')) {
        if (i >= 4 || part.empty()) return false;
        for (char c : part) if (c < '0' || c > '9') return false;
        long v = std::strtol(part.c_str(), nullptr, 10);
        if (v < 0 || v > 255) return false;
        oct[i++] = static_cast<int>(v);
    }
    return i == 4;
}

} // namespace

NetPlan plan_bridge_net(const std::string& subnet_cidr, const std::string& bridge,
                        int host_octet) {
    NetPlan p;
    if (host_octet < 2 || host_octet > 254) return p;   // .1 is the gateway
    const auto slash = subnet_cidr.find('/');
    if (slash == std::string::npos) return p;
    const std::string net = subnet_cidr.substr(0, slash);
    const std::string pfx = subnet_cidr.substr(slash + 1);
    for (char c : pfx) if (c < '0' || c > '9') return p;
    const int prefix = pfx.empty() ? -1 : static_cast<int>(std::strtol(pfx.c_str(), nullptr, 10));
    int oct[4];
    if (prefix != 24 || !parse_ipv4(net, oct)) return p;   // v1: a single /24

    const std::string base = std::to_string(oct[0]) + "." + std::to_string(oct[1]) +
                             "." + std::to_string(oct[2]) + ".";
    p.ok           = true;
    p.bridge       = bridge;
    p.cidr         = subnet_cidr;
    p.prefix       = prefix;
    p.gateway      = base + "1";
    p.container_ip = base + std::to_string(host_octet);
    return p;
}

std::string nft_container_ruleset(const std::string& cidr, const std::string& bridge) {
    std::ostringstream ss;
    // `add table` BEFORE `flush table`: on the very first container run the table
    // doesn't exist yet, and `flush table` on a missing table errors out
    // ("No such file or directory") — which aborts bridge setup and fails the run.
    // `add table` is idempotent (no-op if present), so this makes re-apply safe
    // whether or not the table already exists. Mirrors device_dnat.cpp. We only
    // ever touch our own scoped table, never net-router's iot_router.
    ss << "add table inet iot_containers\n";
    ss << "flush table inet iot_containers\n";
    ss << "table inet iot_containers {\n";
    ss << "    chain postrouting {\n";
    ss << "        type nat hook postrouting priority 100; policy accept;\n";
    ss << "        ip saddr " << cidr << " oifname != \"" << bridge << "\" masquerade\n";
    ss << "    }\n";
    ss << "    chain forward {\n";
    ss << "        type filter hook forward priority 0; policy accept;\n";
    ss << "        iifname \"" << bridge << "\" accept\n";
    ss << "        oifname \"" << bridge << "\" accept\n";
    ss << "    }\n";
    ss << "}\n";
    return ss.str();
}

} // namespace containers
