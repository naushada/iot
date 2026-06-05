#ifndef __server_lwm2m_endpoint_registry_hpp__
#define __server_lwm2m_endpoint_registry_hpp__

/// L21/D1 — In-memory endpoint registry.
///
/// Maps LwM2M endpoint names to tunnel IPs and proxy ports for the
/// multi-tenant cloud server.  Thread-safe (internal mutex).  All
/// three lookup dimensions (ep, tun_ip, proxy_port) are unique.

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace server {
namespace lwm2m {

struct EndpointInfo {
    std::string ep;          // "urn:dev:gateway-42"
    std::string tun_ip;      // "10.9.0.12"
    std::uint16_t proxy_port = 0;  // 5001+
    bool registered = false;       // LwM2M registration active

    EndpointInfo() = default;
    EndpointInfo(std::string ep_, std::string tun_ip_,
                 std::uint16_t proxy_port_, bool registered_)
        : ep(std::move(ep_)), tun_ip(std::move(tun_ip_)),
          proxy_port(proxy_port_), registered(registered_) {}
};

class EndpointRegistry {
public:
    EndpointRegistry() = default;

    /// Add an endpoint.  Returns false when `info.ep`, `info.tun_ip`,
    /// or `info.proxy_port` collides with an existing entry.
    bool add(const EndpointInfo& info);

    /// Remove by endpoint name.  Returns false when not found.
    bool remove(const std::string& ep);

    /// Update the LwM2M registration flag.  Returns false when the
    /// endpoint is not in the registry.
    bool update_state(const std::string& ep, bool registered);

    /// Lookup by endpoint name.  Returns nullptr when not found.
    const EndpointInfo* lookup_by_ep(const std::string& ep) const;

    /// Lookup by tunnel IP.  Returns nullptr when not found.
    const EndpointInfo* lookup_by_tun_ip(const std::string& ip) const;

    /// Lookup by proxy port.  Returns nullptr when not found.
    const EndpointInfo* lookup_by_proxy_port(std::uint16_t port) const;

    /// Number of registered endpoints.
    std::size_t count() const;

    /// Snapshot of all endpoints (copy).
    std::vector<EndpointInfo> list_all() const;

private:
    mutable std::mutex m_mutex;

    // Primary index: endpoint name → full info
    std::unordered_map<std::string, EndpointInfo> m_by_ep;

    // Secondary indexes for reverse lookups (keys only)
    std::unordered_map<std::string, std::string> m_tun_ip_to_ep;   // "10.9.0.10" → "urn:dev:..."
    std::unordered_map<std::uint16_t, std::string> m_port_to_ep;   // 5001 → "urn:dev:..."
};

} // namespace lwm2m
} // namespace server

#endif
