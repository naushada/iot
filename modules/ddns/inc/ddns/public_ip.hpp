#ifndef __ddns_public_ip_hpp__
#define __ddns_public_ip_hpp__

#include <memory>
#include <optional>
#include <string>
#include <vector>

/**
 * @file public_ip.hpp
 * @brief PublicIpDetector interface — resolves the device's current public IPv4.
 *
 * The device only ever sees private/LAN addresses (cell.ip, net.iface.active.ip,
 * wifi.dhcp.ip), so the public IP must be discovered externally. The default
 * strategy queries an HTTPS echo endpoint with ordered fallbacks. Concrete impl
 * lands in modules/ddns/src/public_ip.cpp (FR-3 / #515); PR-1 ships the contract.
 */

namespace ddns {

/// How the public IP is obtained.
enum class IpSource {
    Echo,          ///< HTTPS GET to an echo endpoint (default, portable)
    Dyndns2Auto,   ///< let the provider infer the request source IP (no echo)
    Cloud,         ///< future: cloud publishes the observed wan_ip back to us
};

IpSource parse_ip_source(const std::string& s);   // "echo"|"dyndns2-auto"|"cloud"

/// Resolves the current public IPv4. Reactor-integrated HTTPS lives behind the
/// impl; callers get a synchronous best-effort answer per tick.
struct PublicIpDetector {
    virtual ~PublicIpDetector() = default;

    /// Return the current public IPv4 (dotted-quad) or nullopt on total
    /// failure (all echo endpoints unreachable / no egress / clock invalid).
    virtual std::optional<std::string> detect() = 0;
};

/// Echo-based detector over the given ordered endpoint list (first that answers
/// with a valid IPv4 wins). Empty list → a built-in default set.
std::unique_ptr<PublicIpDetector>
make_echo_detector(std::vector<std::string> endpoints = {});

} // namespace ddns

#endif /* __ddns_public_ip_hpp__ */
