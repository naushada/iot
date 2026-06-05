#include "management.hpp"

namespace server {
namespace lwm2m {

ManagementRouter::ManagementRouter(EndpointRegistry& reg) : m_reg(reg) {}

std::string ManagementRouter::parse_ep(const std::string& query) {
    std::string_view q{query};
    while (!q.empty()) {
        auto amp = q.find('&');
        std::string_view part = (amp == std::string_view::npos) ? q : q.substr(0, amp);
        if (part.starts_with("ep=")) {
            return std::string(part.substr(3));
        }
        if (amp == std::string_view::npos) break;
        q = q.substr(amp + 1);
    }
    return {};
}

const EndpointInfo* ManagementRouter::resolve(const std::string& ep) const {
    return m_reg.lookup_by_ep(ep);
}

bool ManagementRouter::set_registered(const std::string& ep, bool registered) {
    return m_reg.update_state(ep, registered);
}

std::optional<RouteTarget> ManagementRouter::route(const std::string& request) {
    // Parse: "METHOD /path?query"
    auto space = request.find(' ');
    if (space == std::string::npos) return std::nullopt;
    auto path_start = request.find('/', space);
    if (path_start == std::string::npos) return std::nullopt;

    std::string full_path = request.substr(path_start);
    auto qm = full_path.find('?');
    std::string path = (qm != std::string::npos) ? full_path.substr(0, qm) : full_path;

    std::string query = (qm != std::string::npos) ? full_path.substr(qm + 1) : "";
    std::string ep = parse_ep(query);
    if (ep.empty()) return std::nullopt;

    const auto* info = resolve(ep);
    if (!info || !info->registered) return std::nullopt;

    return RouteTarget{info->tun_ip, path};
}

} // namespace lwm2m
} // namespace server
