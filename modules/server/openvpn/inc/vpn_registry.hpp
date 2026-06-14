#ifndef __server_openvpn_vpn_registry_hpp__
#define __server_openvpn_vpn_registry_hpp__

/// L21/D2 — VPN tunnel IP + proxy-port allocator.
///
/// Manages a subnet (default 10.9.0.0/24) from which each device
/// gets a unique tunnel IP, and a proxy-port range (default 5001+)
/// for per-device HTTP forwarding.  Thread-safe.

#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

namespace server {
namespace openvpn {

struct VpnAllocation {
    std::string   ep;          // endpoint name
    std::string   tun_ip;      // assigned tunnel IP
    std::uint16_t proxy_port;  // assigned proxy port
};

class VpnRegistry {
public:
    /// Construct with a subnet and optional port range.
    /// subnet_cidr: e.g. "10.9.0.0/24" — .1 is reserved as gateway
    /// port_start:  first assignable proxy port (default 5001)
    /// port_end:    last assignable proxy port (default 6000)
    explicit VpnRegistry(std::string subnet_cidr = "10.9.0.0/24",
                         std::uint16_t port_start = 5001,
                         std::uint16_t port_end = 6000);

    const std::string& subnet() const { return m_subnet_cidr; }
    std::uint16_t proxy_port_start() const { return m_port_start; }

    /// Allocate IP + port for an endpoint.  Returns nullopt when
    /// the subnet or port range is exhausted.
    std::optional<VpnAllocation> allocate(const std::string& ep);

    /// Restore a previously-persisted allocation at startup (rehydration).
    /// Marks `ip` / `port` as in-use for `ep` so the pools never hand them
    /// out again — unlike allocate(), the caller supplies the exact values
    /// read back from the persisted cloud.endpoints. Empty ip / zero port
    /// are ignored; an already-allocated ip/port is left as-is.
    void reserve(const std::string& ep, const std::string& ip,
                 std::uint16_t port);

    /// Allocate only an IP.  Returns nullopt when exhausted.
    std::optional<std::string> allocate_ip();

    /// Allocate only a port.  Returns nullopt when exhausted.
    std::optional<std::uint16_t> allocate_port();

    /// Release all resources for an endpoint.
    void release(const std::string& ep);

    /// Release a specific IP back to the pool.
    void release_ip(const std::string& ip);

    /// Release a specific port back to the pool.
    void release_port(std::uint16_t port);

    /// Number of currently allocated IPs.
    std::size_t allocated_count() const;

    /// Check whether an IP is within the subnet (excludes .1 and broadcast).
    bool contains_ip(const std::string& ip) const;

private:
    std::string m_subnet_cidr;
    std::uint16_t m_port_start;
    std::uint16_t m_port_end;

    mutable std::mutex m_mutex;

    std::set<std::string> m_free_ips;       // sorted — first is lowest
    std::set<std::uint16_t> m_free_ports;   // sorted — first is lowest
    std::set<std::string> m_allocated_ips;
    std::set<std::uint16_t> m_allocated_ports;

    std::unordered_map<std::string, VpnAllocation> m_ep_to_alloc;

    /// Parse CIDR into (base, prefix_len).
    static std::pair<std::uint32_t, int> parse_cidr(const std::string& cidr);

    /// Convert IP string to uint32.
    static std::uint32_t ip_to_u32(const std::string& ip);

    /// Convert uint32 to IP string.
    static std::string u32_to_ip(std::uint32_t n);

    /// Populate the free pool for the given subnet range.
    void init_pool();
};

} // namespace openvpn
} // namespace server

#endif
