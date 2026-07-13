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
    std::uint16_t proxy_port;  // assigned proxy port; 0 == none (pool exhausted)
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

    /// Allocate a tunnel IP (+ a proxy port if one is free) for an endpoint.
    ///
    /// Returns nullopt ONLY when the IP subnet is exhausted. An exhausted PORT
    /// pool is not an error: the allocation succeeds with `proxy_port == 0`,
    /// meaning "no legacy DNAT forward for this device" — it is still reachable
    /// over the path-scoped proxy (/dev/<ep>/), which needs no port. A port is
    /// a 16-bit resource and cannot cover a large fleet; onboarding must not
    /// depend on one. See apps/docs/tdd-cloud-scale-1m-devices.md §C1/P0b.
    std::optional<VpnAllocation> allocate(const std::string& ep);

    /// Multi-tenant (P3c): allocate a tunnel IP from `subnet_cidr` (a tenant's
    /// /24) plus a proxy port if one is free, avoiding any IP already handed
    /// out. Returns nullopt when the TENANT SUBNET is exhausted; an exhausted
    /// port pool yields `proxy_port == 0`, exactly as in allocate(). The tenant
    /// IP is tracked as allocated but is never returned to the base free pool on
    /// release (it is re-derivable from the tenant subnet on demand).
    std::optional<VpnAllocation> allocate_in_subnet(const std::string& ep,
                                                    const std::string& subnet_cidr);

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

    /// Order dotted-quad IPs NUMERICALLY, not lexicographically.
    ///
    /// A plain std::set<std::string> compares byte-wise, so "10.9.0.10" sorts
    /// BEFORE "10.9.0.2" and the pool hands out .10 as its first address. That
    /// is still correct (every IP is unique) but it is not what the allocator
    /// claims — "first is lowest" — and it is not what six of this class's own
    /// unit tests assert. With a /16 pool (65534 hosts) the string order is
    /// wild enough to be actively confusing when reading `cloud.endpoints`.
    struct IpLess {
        bool operator()(const std::string& a, const std::string& b) const {
            return ip_to_u32(a) < ip_to_u32(b);
        }
    };

    std::set<std::string, IpLess> m_free_ips;   // sorted numerically — first is lowest
    std::set<std::uint16_t> m_free_ports;       // sorted — first is lowest
    std::set<std::string> m_allocated_ips;      // membership only; order irrelevant
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

    /// Lock-free release helpers — caller MUST already hold m_mutex.
    /// release() holds the lock for the whole endpoint teardown, so it
    /// must NOT call the public release_ip()/release_port() (which re-lock
    /// the non-recursive m_mutex → self-deadlock); it calls these instead.
    void release_ip_locked(const std::string& ip);
    void release_port_locked(std::uint16_t port);

    /// Take the lowest free proxy port and mark it allocated, or return 0 when
    /// the pool is dry (0 is the "no proxy port" sentinel — it is never a valid
    /// allocatable port, since the range is >= 1024). Caller MUST hold m_mutex.
    std::uint16_t take_port_locked();
};

} // namespace openvpn
} // namespace server

#endif
