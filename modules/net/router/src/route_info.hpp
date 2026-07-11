#ifndef __net_router_route_info_hpp__
#define __net_router_route_info_hpp__

/// Live routing snapshot for the device-ui Routing page: the kernel
/// routing table, the interface list and the active resolvers, each
/// serialised as a compact JSON string ready to publish to ds
/// (net.routes / net.ifaces / net.dns).
///
/// Split like iface_monitor: pure `parse_*` functions over command
/// output (host-testable), thin `*_json` wrappers that shell out via
/// the injected Runner.

#include <string>

#include "shell.hpp"

namespace net_router::route_info {

/// `ip -j route show` body → JSON array string
/// [{"dst","gateway","dev","proto","scope","prefsrc","metric"}].
/// Unparseable body → "[]".
std::string parse_routes(const std::string& body);

/// `ip -j addr show` body → JSON array string
/// [{"name","state","mac","ip"}] (first routable IPv4 per iface, "" if
/// none). Unparseable body → "[]".
std::string parse_ifaces(const std::string& body);

/// resolv.conf text → comma-joined nameserver list ("" if none).
std::string parse_resolv_conf(const std::string& text);

/// Runner-backed wrappers used by the daemon poll loop.
std::string routes_json(shell::Runner runner);
std::string ifaces_json(shell::Runner runner);
/// Reads /etc/resolv.conf (or `path` when given, for tests).
std::string dns_csv(const std::string& path = "/etc/resolv.conf");

} // namespace net_router::route_info

#endif /* __net_router_route_info_hpp__ */
