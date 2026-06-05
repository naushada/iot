#include "server/web/proxy.hpp"
#include "server/lwm2m/endpoint_registry.hpp"

namespace server {
namespace web {

DeviceProxy::DeviceProxy(lwm2m::EndpointRegistry& reg) : m_reg(reg) {}

std::optional<ProxyTarget> DeviceProxy::resolve(const std::string& path) {
    // path is "/<ep>/..." or "/<ep>"
    if (path.empty() || path[0] != '/') return std::nullopt;

    std::string_view p{path};
    p.remove_prefix(1);  // skip leading '/'

    auto slash = p.find('/');
    std::string ep = (slash != std::string_view::npos)
        ? std::string(p.substr(0, slash))
        : std::string(p);

    if (ep.empty()) return std::nullopt;

    const auto* info = m_reg.lookup_by_ep(ep);
    if (!info) return std::nullopt;

    std::string tail = (slash != std::string_view::npos)
        ? std::string(p.substr(slash + 1))
        : "";

    ProxyTarget target;
    target.target_url = "http://" + info->tun_ip + ":80/";
    if (!tail.empty()) target.target_url += tail;
    return target;
}

} // namespace web
} // namespace server
