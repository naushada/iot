#include "endpoint_registry.hpp"

namespace server {
namespace lwm2m {

bool EndpointRegistry::add(const EndpointInfo& info) {
    std::lock_guard<std::mutex> lk(m_mutex);

    if (m_by_ep.count(info.ep)) return false;               // duplicate ep
    if (m_tun_ip_to_ep.count(info.tun_ip)) return false;    // duplicate tun IP

    // proxy_port == 0 means "no proxy port" (the port pool was exhausted — see
    // VpnRegistry::allocate). It is NOT an allocation, so it must not be indexed
    // and must not collide: EVERY portless device carries 0, so treating 0 as a
    // real port would reject the second one as a duplicate and — because
    // BootstrapProvisioner rolls the VPN allocation back when add() fails —
    // would block it from onboarding at all. Index only real ports.
    const bool has_port = (info.proxy_port != 0);
    if (has_port && m_port_to_ep.count(info.proxy_port)) return false;  // duplicate port

    m_by_ep[info.ep] = info;
    m_tun_ip_to_ep[info.tun_ip] = info.ep;
    if (has_port) m_port_to_ep[info.proxy_port] = info.ep;
    return true;
}

bool EndpointRegistry::remove(const std::string& ep) {
    std::lock_guard<std::mutex> lk(m_mutex);

    auto it = m_by_ep.find(ep);
    if (it == m_by_ep.end()) return false;

    m_tun_ip_to_ep.erase(it->second.tun_ip);
    m_port_to_ep.erase(it->second.proxy_port);
    m_by_ep.erase(it);
    return true;
}

bool EndpointRegistry::update_state(const std::string& ep, bool registered) {
    std::lock_guard<std::mutex> lk(m_mutex);

    auto it = m_by_ep.find(ep);
    if (it == m_by_ep.end()) return false;
    it->second.registered = registered;
    return true;
}

bool EndpointRegistry::update_state(const std::string& ep, bool registered,
                                    std::int64_t last_seen_unix) {
    std::lock_guard<std::mutex> lk(m_mutex);

    auto it = m_by_ep.find(ep);
    if (it == m_by_ep.end()) return false;
    it->second.registered     = registered;
    it->second.last_seen_unix = last_seen_unix;
    return true;
}

bool EndpointRegistry::update_dev_tun_ip(const std::string& ep,
                                         const std::string& ip) {
    std::lock_guard<std::mutex> lk(m_mutex);

    auto it = m_by_ep.find(ep);
    if (it == m_by_ep.end()) return false;          // unknown endpoint
    if (it->second.dev_tun_ip == ip) return false;  // unchanged
    // dev_tun_ip is not a unique lookup key (no reverse index), so just store
    // it — the registry-allocated tun_ip + its index are left untouched.
    it->second.dev_tun_ip = ip;
    return true;
}

bool EndpointRegistry::update_wan_ip(const std::string& ep,
                                     const std::string& ip) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_by_ep.find(ep);
    if (it == m_by_ep.end()) return false;       // unknown endpoint
    if (it->second.wan_ip == ip) return false;   // unchanged
    it->second.wan_ip = ip;                       // display only, no index
    return true;
}

bool EndpointRegistry::update_version(const std::string& ep,
                                      const std::string& version) {
    std::lock_guard<std::mutex> lk(m_mutex);

    auto it = m_by_ep.find(ep);
    if (it == m_by_ep.end()) return false;                  // unknown endpoint
    if (it->second.installed_version == version) return false;  // unchanged
    it->second.installed_version = version;
    return true;
}

bool EndpointRegistry::update_lan_ip(const std::string& ep,
                                     const std::string& ip) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_by_ep.find(ep);
    if (it == m_by_ep.end()) return false;       // unknown endpoint
    if (it->second.lan_ip == ip) return false;   // unchanged
    it->second.lan_ip = ip;                       // display only, no index
    return true;
}

bool EndpointRegistry::update_tenant(const std::string& ep,
                                     const std::string& tenant) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_by_ep.find(ep);
    if (it == m_by_ep.end()) return false;       // unknown endpoint
    if (it->second.tenant == tenant) return false;  // unchanged
    it->second.tenant = tenant;                   // display/scoping only, no index
    return true;
}

bool EndpointRegistry::update_reg_meta(const std::string& ep,
                                       std::uint32_t lifetime,
                                       const std::string& location) {
    std::lock_guard<std::mutex> lk(m_mutex);

    auto it = m_by_ep.find(ep);
    if (it == m_by_ep.end()) return false;                  // unknown endpoint
    bool changed = false;
    if (lifetime != 0 && it->second.lifetime != lifetime) {
        it->second.lifetime = lifetime;
        changed = true;
    }
    if (!location.empty() && it->second.location != location) {
        it->second.location = location;
        changed = true;
    }
    return changed;
}

const EndpointInfo* EndpointRegistry::lookup_by_ep(const std::string& ep) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_by_ep.find(ep);
    return (it != m_by_ep.end()) ? &it->second : nullptr;
}

const EndpointInfo* EndpointRegistry::lookup_by_tun_ip(const std::string& ip) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_tun_ip_to_ep.find(ip);
    if (it == m_tun_ip_to_ep.end()) return nullptr;
    auto eit = m_by_ep.find(it->second);
    return (eit != m_by_ep.end()) ? &eit->second : nullptr;
}

const EndpointInfo* EndpointRegistry::lookup_by_proxy_port(std::uint16_t port) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_port_to_ep.find(port);
    if (it == m_port_to_ep.end()) return nullptr;
    auto eit = m_by_ep.find(it->second);
    return (eit != m_by_ep.end()) ? &eit->second : nullptr;
}

std::size_t EndpointRegistry::count() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_by_ep.size();
}

std::vector<EndpointInfo> EndpointRegistry::list_all() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    std::vector<EndpointInfo> out;
    out.reserve(m_by_ep.size());
    for (const auto& kv : m_by_ep) out.push_back(kv.second);
    return out;
}

} // namespace lwm2m
} // namespace server
