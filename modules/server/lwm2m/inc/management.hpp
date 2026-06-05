#ifndef __server_lwm2m_management_hpp__
#define __server_lwm2m_management_hpp__

/// L21/D4 — LwM2M DM Server: registration + request routing.
///
/// Parses CoAP URIs with ?ep=<endpoint> query parameter, resolves
/// the endpoint in the registry, and returns the target tunnel IP
/// for forwarding the LwM2M operation.

#include "endpoint_registry.hpp"
#include <optional>
#include <string>

namespace server {
namespace lwm2m {

struct RouteTarget {
    std::string target_ip;   // tunnel IP to forward to
    std::string path;        // CoAP path (e.g. "/3/0/6")
};

class ManagementRouter {
public:
    explicit ManagementRouter(EndpointRegistry& reg);

    /// Parse ?ep= value from a CoAP URI query string.
    static std::string parse_ep(const std::string& query);

    /// Resolve an endpoint name to its registry entry.
    const EndpointInfo* resolve(const std::string& ep) const;

    /// Set the LwM2M registration flag for an endpoint.
    bool set_registered(const std::string& ep, bool registered);

    /// Route a CoAP request: parse ep, resolve, return target.
    /// `request` is "METHOD /path?query" e.g. "GET /3/0/6?ep=dev-1".
    std::optional<RouteTarget> route(const std::string& request);

private:
    EndpointRegistry& m_reg;
};

} // namespace lwm2m
} // namespace server
#endif
