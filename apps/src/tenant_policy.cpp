/// Multi-tenant cloud — P1 core primitives. See tenant_policy.hpp +
/// apps/docs/tdd-multi-tenant-cloud.md.

#include "tenant_policy.hpp"

#include <algorithm>
#include <cctype>

#include "nlohmann/json.hpp"

#include "psk_gen.hpp"   // iot::sha256_hex

namespace iot {

namespace {
const char  kDmPrefix[] = "rpi";
const char  kDmSuffix[] = "@cloud.local";          // legacy (default tenant)
const char  kDmDot[]    = ".cloud.local";          // multi-tenant suffix

bool is_slug_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
}
} // namespace

bool valid_tenant_id(const std::string& id) {
    if (id.empty() || id.size() > 32) return false;
    if (id.front() == '-' || id.back() == '-') return false;
    return std::all_of(id.begin(), id.end(), is_slug_char);
}

EndpointId split_endpoint(const std::string& ep) {
    const auto colon = ep.find(':');
    if (colon == std::string::npos)
        return {kDefaultTenant, ep};
    const std::string tenant = ep.substr(0, colon);
    const std::string serial = ep.substr(colon + 1);
    // A bad prefix must not silently land a device in the wrong tenant — treat
    // the whole string as a default-tenant serial instead.
    if (!valid_tenant_id(tenant) || serial.empty())
        return {kDefaultTenant, ep};
    return {tenant, serial};
}

std::string join_endpoint(const std::string& tenant, const std::string& serial) {
    if (tenant.empty() || tenant == kDefaultTenant) return serial;
    return tenant + ":" + serial;
}

std::string bs_identity(const std::string& tenant, const std::string& serial) {
    const std::string in = (tenant.empty() || tenant == kDefaultTenant)
                               ? serial
                               : (tenant + ":" + serial);
    return sha256_hex(in).substr(0, 32);
}

std::string dm_identity(const std::string& tenant, const std::string& serial) {
    if (tenant.empty() || tenant == kDefaultTenant)
        return std::string(kDmPrefix) + serial + kDmSuffix;
    return std::string(kDmPrefix) + serial + "@" + tenant + kDmDot;
}

EndpointId parse_dm_identity(const std::string& identity) {
    const std::size_t plen = sizeof(kDmPrefix) - 1;   // "rpi"
    if (identity.size() <= plen) return {"", ""};
    if (identity.compare(0, plen, kDmPrefix) != 0) return {"", ""};
    const auto at = identity.find('@', plen);
    if (at == std::string::npos || at == plen) return {"", ""};   // empty serial
    const std::string serial = identity.substr(plen, at - plen);
    const std::string domain = identity.substr(at + 1);

    if (domain == "cloud.local")                       // legacy / default
        return {kDefaultTenant, serial};

    const std::size_t dlen = sizeof(kDmDot) - 1;       // ".cloud.local"
    if (domain.size() <= dlen) return {"", ""};
    if (domain.compare(domain.size() - dlen, dlen, kDmDot) != 0) return {"", ""};
    const std::string tenant = domain.substr(0, domain.size() - dlen);
    if (!valid_tenant_id(tenant)) return {"", ""};
    return {tenant, serial};
}

std::string row_tenant(const std::string& row_json) {
    auto j = nlohmann::json::parse(row_json, nullptr, /*allow_exceptions*/false);
    if (!j.is_object()) return kDefaultTenant;
    const std::string t = j.value("tenant", std::string());
    return t.empty() ? std::string(kDefaultTenant) : t;
}

std::string filter_rows_by_tenant(const std::string& array_json,
                                  const std::string& tenant) {
    auto arr = nlohmann::json::parse(array_json, nullptr, false);
    nlohmann::json out = nlohmann::json::array();
    if (arr.is_array()) {
        for (const auto& e : arr) {
            if (!e.is_object()) continue;
            const std::string t = e.value("tenant", std::string());
            const std::string et = t.empty() ? std::string(kDefaultTenant) : t;
            if (et == tenant) out.push_back(e);
        }
    }
    return out.dump();
}

std::optional<TenantInfo> find_tenant(const std::string& tenants_json,
                                      const std::string& id) {
    auto arr = nlohmann::json::parse(tenants_json, nullptr, false);
    if (!arr.is_array()) return std::nullopt;
    for (const auto& e : arr) {
        if (!e.is_object()) continue;
        if (e.value("id", std::string()) != id) continue;
        TenantInfo t;
        t.id          = id;
        t.name        = e.value("name", std::string());
        t.vpn_subnet  = e.value("vpn.subnet", std::string());
        t.dm_uri      = e.value("dm.uri", std::string());
        t.proxy_start = static_cast<std::uint16_t>(
            e.value("proxy.port.start", 0));
        t.proxy_end   = static_cast<std::uint16_t>(
            e.value("proxy.port.end", 0));
        t.active      = e.value("status", std::string("active")) == "active";
        return t;
    }
    return std::nullopt;
}

std::vector<std::string> list_tenant_ids(const std::string& tenants_json) {
    std::vector<std::string> ids;
    auto arr = nlohmann::json::parse(tenants_json, nullptr, false);
    if (arr.is_array()) {
        for (const auto& e : arr) {
            if (!e.is_object()) continue;
            const std::string id = e.value("id", std::string());
            if (!id.empty()) ids.push_back(id);
        }
    }
    return ids;
}

} // namespace iot
