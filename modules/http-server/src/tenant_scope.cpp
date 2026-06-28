/// Multi-tenant console scoping helpers. See tenant_scope.hpp.

#include "tenant_scope.hpp"

#include "auth.hpp"

#include <nlohmann/json.hpp>

namespace http_server {

std::string request_tenant(const std::map<std::string, std::string>& headers,
                           SessionStore* auth) {
    if (!auth) return "*";                         // auth not wired → no filtering
    const std::string token =
        extract_session_cookie(headers, auth->cookie_name());
    const auto* s = token.empty() ? nullptr : auth->validate(token);
    return s ? s->tenant : std::string("default");
}

std::string scope_endpoints_json(const std::string& value,
                                 const std::string& tenant) {
    if (tenant == "*") return value;               // platform operator sees all
    auto arr = nlohmann::json::parse(value, nullptr, /*allow_exceptions*/false);
    if (!arr.is_array()) return value;             // not an endpoint array
    nlohmann::json out = nlohmann::json::array();
    for (const auto& e : arr) {
        if (!e.is_object()) continue;
        const std::string t = e.value("tenant", std::string());
        const std::string et = t.empty() ? std::string("default") : t;
        if (et == tenant) out.push_back(e);
    }
    return out.dump();
}

} // namespace http_server
