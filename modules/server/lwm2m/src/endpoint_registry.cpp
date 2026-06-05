#include "endpoint_registry.hpp"

namespace server {
namespace lwm2m {

bool EndpointRegistry::add(const EndpointInfo& info) {
    std::lock_guard<std::mutex> lk(m_mutex);

    if (m_by_ep.count(info.ep)) return false;               // duplicate ep
    if (m_tun_ip_to_ep.count(info.tun_ip)) return false;    // duplicate tun IP
    if (m_port_to_ep.count(info.proxy_port)) return false;  // duplicate port

    m_by_ep[info.ep] = info;
    m_tun_ip_to_ep[info.tun_ip] = info.ep;
    m_port_to_ep[info.proxy_port] = info.ep;
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
