/// Multi-tenant VPN — pure subnet math + nft isolation rules.
/// See tenant_subnet.hpp + apps/docs/tdd-multi-tenant-cloud.md (P3).

#include "tenant_subnet.hpp"

#include <arpa/inet.h>

#include <cstdint>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

namespace server { namespace openvpn {

namespace {

// Parse "a.b.c.d/n" → (host-order base, prefix). Returns prefix = -1 on error.
std::pair<std::uint32_t, int> parse_cidr(const std::string& cidr) {
    const auto slash = cidr.find('/');
    if (slash == std::string::npos) return {0, -1};
    const std::string ip = cidr.substr(0, slash);
    int prefix = -1;
    try { prefix = std::stoi(cidr.substr(slash + 1)); }
    catch (...) { return {0, -1}; }
    if (prefix < 0 || prefix > 32) return {0, -1};
    in_addr a{};
    if (::inet_pton(AF_INET, ip.c_str(), &a) != 1) return {0, -1};
    return {ntohl(a.s_addr), prefix};
}

std::string u32_to_ip(std::uint32_t v) {
    in_addr a{};
    a.s_addr = htonl(v);
    char buf[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return buf;
}

std::uint32_t mask_of(int prefix) {
    return prefix == 0 ? 0U : (~0U << (32 - prefix));
}

} // namespace

bool subnets_overlap(const std::string& a, const std::string& b) {
    auto [ba, pa] = parse_cidr(a);
    auto [bb, pb] = parse_cidr(b);
    if (pa < 0 || pb < 0) return false;
    const std::uint32_t na = ba & mask_of(pa), bca = na | ~mask_of(pa);
    const std::uint32_t nb = bb & mask_of(pb), bcb = nb | ~mask_of(pb);
    return na <= bcb && nb <= bca;            // ranges [na,bca] and [nb,bcb] intersect
}

std::string allocate_tenant_subnet(const std::string& pool_cidr,
                                   const std::vector<std::string>& used,
                                   int tenant_prefix) {
    auto [pbase, pprefix] = parse_cidr(pool_cidr);
    if (pprefix < 0 || tenant_prefix < pprefix || tenant_prefix > 30)
        return {};

    const std::uint32_t pnet  = pbase & mask_of(pprefix);
    const std::uint32_t pbcast = pnet | ~mask_of(pprefix);
    const std::uint32_t step  = 1U << (32 - tenant_prefix);   // block size

    for (std::uint64_t net = pnet; net + step - 1 <= pbcast; net += step) {
        const std::string cand =
            u32_to_ip(static_cast<std::uint32_t>(net)) + "/" +
            std::to_string(tenant_prefix);
        bool clash = false;
        for (const auto& u : used) {
            if (subnets_overlap(cand, u)) { clash = true; break; }
        }
        if (!clash) return cand;
    }
    return {};   // pool exhausted
}

std::string build_tenant_isolation_rules(
    const std::vector<std::string>& tenant_subnets) {
    // Keep only well-formed, distinct subnets.
    std::vector<std::string> nets;
    for (const auto& s : tenant_subnets) {
        if (parse_cidr(s).second < 0) continue;
        bool dup = false;
        for (const auto& n : nets) if (n == s) { dup = true; break; }
        if (!dup) nets.push_back(s);
    }

    std::ostringstream os;
    os << "table ip iot_tenant_isol {\n";
    os << "  chain forward {\n";
    os << "    type filter hook forward priority -10; policy accept;\n";
    // Pairwise cross-tenant drops (A->B for every A != B). Same-tenant and
    // tenant<->cloud traffic falls through to the accept policy.
    for (std::size_t i = 0; i < nets.size(); ++i)
        for (std::size_t j = 0; j < nets.size(); ++j)
            if (i != j)
                os << "    ip saddr " << nets[i]
                   << " ip daddr " << nets[j] << " drop\n";
    os << "  }\n";
    os << "}\n";
    return os.str();
}

std::pair<std::string, bool> assign_missing_subnets(
    const std::string& tenants_json,
    const std::string& pool_cidr,
    int tenant_prefix) {
    auto arr = nlohmann::json::parse(tenants_json, nullptr, /*exceptions*/false);
    if (!arr.is_array()) return {tenants_json, false};

    // Seed "used" with every subnet already assigned.
    std::vector<std::string> used;
    for (const auto& t : arr)
        if (t.is_object()) {
            const std::string s = t.value("vpn.subnet", std::string());
            if (!s.empty()) used.push_back(s);
        }

    bool changed = false;
    for (auto& t : arr) {
        if (!t.is_object()) continue;
        if (!t.value("vpn.subnet", std::string()).empty()) continue;  // has one
        const std::string s =
            allocate_tenant_subnet(pool_cidr, used, tenant_prefix);
        if (s.empty()) continue;            // pool exhausted — leave unassigned
        t["vpn.subnet"] = s;
        used.push_back(s);
        changed = true;
    }
    return {arr.dump(), changed};
}

bool tenant_at_capacity(const std::string& tenants_json,
                        const std::string& creds_json,
                        const std::string& tenant,
                        const std::string& candidate_serial) {
    // The tenant's cap (0 / absent ⇒ unlimited).
    long max_devices = 0;
    {
        auto ts = nlohmann::json::parse(tenants_json, nullptr, false);
        if (ts.is_array())
            for (const auto& t : ts)
                if (t.is_object() && t.value("id", std::string()) == tenant) {
                    max_devices = t.value("max.devices", 0);
                    break;
                }
    }
    if (max_devices <= 0) return false;

    // Current count of this tenant's credential rows; a re-provision of an
    // already-present serial doesn't grow it, so it's never blocked.
    long count = 0;
    auto cr = nlohmann::json::parse(creds_json, nullptr, false);
    if (cr.is_array())
        for (const auto& c : cr) {
            if (!c.is_object()) continue;
            const std::string ct = c.value("tenant", std::string());
            const std::string et = ct.empty() ? std::string("default") : ct;
            if (et != tenant) continue;
            if (c.value("serial", std::string()) == candidate_serial)
                return false;            // already provisioned → allowed
            ++count;
        }
    return count >= max_devices;
}

}} // namespace server::openvpn
