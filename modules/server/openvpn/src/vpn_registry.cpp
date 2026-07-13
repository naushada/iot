#include "vpn_registry.hpp"

#include "tenant_subnet.hpp"   // allocate_ip_in_subnet (multi-tenant P3c)

#include <algorithm>
#include <arpa/inet.h>
#include <cstring>
#include <vector>

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

    // Port pool. Port 0 is the "no proxy port" sentinel (see take_port_locked)
    // and must never enter the free pool, so a caller passing port_start = 0
    // starts at 1. m_port_end == 0 means "no port pool at all".
    if (m_port_end != 0) {
        for (std::uint16_t p = (m_port_start == 0) ? 1 : m_port_start;
             p <= m_port_end; ++p) {
            m_free_ports.insert(p);
        }
    }
}

// ── Allocation ─────────────────────────────────────────────────────

std::optional<VpnAllocation> VpnRegistry::allocate(const std::string& ep) {
    std::lock_guard<std::mutex> lk(m_mutex);

    // The tunnel IP is REQUIRED — without one the device has no VPN presence at
    // all. The proxy port is OPTIONAL: it only feeds the legacy per-device DNAT
    // (cloud:<port> -> <tun_ip>:8080), which the path-scoped proxy (/dev/<ep>/)
    // has superseded. The port pool is a 16-bit resource and can never cover a
    // large fleet, so running it dry must NOT block onboarding: the device gets
    // proxy_port = 0, no DNAT rule is emitted for it (rebuild_device_dnat and
    // build_device_dnat_ruleset both skip port 0), and it is reached over the
    // path proxy like every other device.
    if (m_free_ips.empty()) return std::nullopt;

    auto ip_it = m_free_ips.begin();

    VpnAllocation a;
    a.ep         = ep;
    a.tun_ip     = *ip_it;
    a.proxy_port = take_port_locked();   // 0 when the pool is exhausted

    m_free_ips.erase(ip_it);
    m_allocated_ips.insert(a.tun_ip);
    m_ep_to_alloc[ep] = a;

    return a;
}

std::optional<VpnAllocation>
VpnRegistry::allocate_in_subnet(const std::string& ep,
                                const std::string& subnet_cidr) {
    std::lock_guard<std::mutex> lk(m_mutex);

    // Pick a host IP from the tenant subnet, avoiding every IP this registry has
    // already handed out (base pool + all tenants) so two tenants whose /24s
    // were mis-configured to overlap still never collide.
    std::vector<std::string> used(m_allocated_ips.begin(), m_allocated_ips.end());
    const std::string ip = allocate_ip_in_subnet(subnet_cidr, used);
    if (ip.empty()) return std::nullopt;   // tenant subnet exhausted — fatal

    VpnAllocation a;
    a.ep         = ep;
    a.tun_ip     = ip;            // from the tenant /24, NOT the base free pool
    a.proxy_port = take_port_locked();     // 0 when exhausted — NOT fatal, see allocate()

    m_allocated_ips.insert(ip);   // tracked as in-use; not drawn from m_free_ips
    m_ep_to_alloc[ep] = a;

    return a;
}

std::uint16_t VpnRegistry::take_port_locked() {
    if (m_free_ports.empty()) return 0;   // 0 == "no proxy port" sentinel
    auto it = m_free_ports.begin();
    const std::uint16_t port = *it;
    m_free_ports.erase(it);
    m_allocated_ports.insert(port);
    return port;
}

void VpnRegistry::reserve(const std::string& ep, const std::string& ip,
                          std::uint16_t port) {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!ip.empty()) {
        m_free_ips.erase(ip);
        m_allocated_ips.insert(ip);
    }
    if (port != 0) {
        m_free_ports.erase(port);
        m_allocated_ports.insert(port);
    }
    VpnAllocation a;
    a.ep         = ep;
    a.tun_ip     = ip;
    a.proxy_port = port;
    m_ep_to_alloc[ep] = a;
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

    // Use the lock-free variants — m_mutex is already held here. Calling the
    // public release_ip()/release_port() would re-lock the non-recursive
    // m_mutex and self-deadlock the whole iot-cloudd main loop (every
    // endpoint deprovision goes through here).
    release_ip_locked(it->second.tun_ip);
    release_port_locked(it->second.proxy_port);
    m_ep_to_alloc.erase(it);
}

void VpnRegistry::release_ip_locked(const std::string& ip) {
    if (!m_allocated_ips.erase(ip)) return;
    // Only return it to the base free pool if it belongs to THIS registry's own
    // subnet. A tenant IP (handed out by allocate_in_subnet from a tenant /24
    // outside the base pool) must NOT pollute the base allocator — dropping it
    // from m_allocated_ips is enough (it's re-derivable from the tenant subnet).
    const auto cidr = parse_cidr(m_subnet_cidr);
    const int prefix = cidr.second;
    if (prefix > 0 && prefix < 32) {
        const std::uint32_t mask  = ~0u << (32 - prefix);
        const std::uint32_t net   = cidr.first & mask;
        const std::uint32_t bcast = net | ~mask;
        const std::uint32_t v     = ip_to_u32(ip);
        if (v > net && v < bcast) m_free_ips.insert(ip);   // base host range
        return;
    }
    m_free_ips.insert(ip);   // unusual prefix → preserve legacy behaviour
}

void VpnRegistry::release_port_locked(std::uint16_t port) {
    if (m_allocated_ports.erase(port)) {
        m_free_ports.insert(port);
    }
}

void VpnRegistry::release_ip(const std::string& ip) {
    std::lock_guard<std::mutex> lk(m_mutex);
    release_ip_locked(ip);
}

void VpnRegistry::release_port(std::uint16_t port) {
    std::lock_guard<std::mutex> lk(m_mutex);
    release_port_locked(port);
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
