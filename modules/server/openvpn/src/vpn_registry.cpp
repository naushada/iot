#include "server/openvpn/vpn_registry.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cstring>

namespace server {
namespace openvpn {

// ── IP conversion helpers ──────────────────────────────────────────

std::uint32_t VpnRegistry::ip_to_u32(const std::string& ip) {
    std::uint32_t n = 0;
    ::inet_pton(AF_INET, ip.c_str(), &n);
    return ntohl(n);
}

std::string VpnRegistry::u32_to_ip(std::uint32_t n) {
    n = htonl(n);
    char buf[INET_ADDRSTRLEN];
    ::inet_ntop(AF_INET, &n, buf, sizeof(buf));
    return buf;
}

std::pair<std::uint32_t, int> VpnRegistry::parse_cidr(const std::string& cidr) {
    auto slash = cidr.find('/');
    std::string ip_str = cidr.substr(0, slash);
    int prefix = std::stoi(cidr.substr(slash + 1));
    return {ip_to_u32(ip_str), prefix};
}

// ── Constructor ────────────────────────────────────────────────────

VpnRegistry::VpnRegistry(std::string subnet_cidr,
                         std::uint16_t port_start,
                         std::uint16_t port_end)
    : m_subnet_cidr(std::move(subnet_cidr))
    , m_port_start(port_start)
    , m_port_end(port_end)
{
    init_pool();
}

void VpnRegistry::init_pool() {
    auto [base, prefix] = parse_cidr(m_subnet_cidr);
    std::uint32_t mask = (prefix == 0) ? 0 : (~0U << (32 - prefix));
    std::uint32_t network = base & mask;
    std::uint32_t broadcast = network | ~mask;

    // Allocatable range: network+2 .. broadcast-1 (skip .0 and .1 gateway)
    std::uint32_t first = (prefix <= 30) ? network + 2 : network + 1;
    std::uint32_t last  = (prefix <= 30) ? broadcast - 1 : broadcast;
    if (first > last) return;  // no allocatable IPs

    for (std::uint32_t ip = first; ip <= last; ++ip) {
        m_free_ips.insert(u32_to_ip(ip));
    }

    // Port pool
    for (std::uint16_t p = m_port_start; p <= m_port_end; ++p) {
        m_free_ports.insert(p);
    }
}

// ── Allocation ─────────────────────────────────────────────────────

std::optional<VpnAllocation> VpnRegistry::allocate(const std::string& ep) {
    std::lock_guard<std::mutex> lk(m_mutex);

    if (m_free_ips.empty() || m_free_ports.empty()) return std::nullopt;

    auto ip_it = m_free_ips.begin();
    auto port_it = m_free_ports.begin();

    VpnAllocation a;
    a.ep         = ep;
    a.tun_ip     = *ip_it;
    a.proxy_port = *port_it;

    m_free_ips.erase(ip_it);
    m_free_ports.erase(port_it);
    m_allocated_ips.insert(a.tun_ip);
    m_allocated_ports.insert(a.proxy_port);
    m_ep_to_alloc[ep] = a;

    return a;
}

std::optional<std::string> VpnRegistry::allocate_ip() {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_free_ips.empty()) return std::nullopt;
    auto it = m_free_ips.begin();
    std::string ip = *it;
    m_free_ips.erase(it);
    m_allocated_ips.insert(ip);
    return ip;
}

std::optional<std::uint16_t> VpnRegistry::allocate_port() {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_free_ports.empty()) return std::nullopt;
    auto it = m_free_ports.begin();
    std::uint16_t port = *it;
    m_free_ports.erase(it);
    m_allocated_ports.insert(port);
    return port;
}

// ── Release ────────────────────────────────────────────────────────

void VpnRegistry::release(const std::string& ep) {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_ep_to_alloc.find(ep);
    if (it == m_ep_to_alloc.end()) return;

    release_ip(it->second.tun_ip);
    release_port(it->second.proxy_port);
    m_ep_to_alloc.erase(it);
}

void VpnRegistry::release_ip(const std::string& ip) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_allocated_ips.erase(ip)) {
        m_free_ips.insert(ip);
    }
}

void VpnRegistry::release_port(std::uint16_t port) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_allocated_ports.erase(port)) {
        m_free_ports.insert(port);
    }
}

// ── Queries ────────────────────────────────────────────────────────

std::size_t VpnRegistry::allocated_count() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_allocated_ips.size();
}

bool VpnRegistry::contains_ip(const std::string& ip) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (ip == u32_to_ip(ip_to_u32(m_subnet_cidr.substr(0, m_subnet_cidr.find('/'))) + 1))
        return false;  // gateway
    auto [base, prefix] = parse_cidr(m_subnet_cidr);
    std::uint32_t mask = (prefix == 0) ? 0 : (~0U << (32 - prefix));
    std::uint32_t network = base & mask;
    std::uint32_t broadcast = network | ~mask;
    std::uint32_t ip_n = ip_to_u32(ip);
    return ip_n > network && ip_n < broadcast;
}

} // namespace openvpn
} // namespace server
