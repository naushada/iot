#ifndef __server_web_proxy_hpp__
#define __server_web_proxy_hpp__

/// L21/D5 — HTTP reverse proxy to device UIs.
///
/// Resolves /<ep>/ → tunnel IP and returns the target for
/// the HTTP proxy handler in iot-httpd.

#include <optional>
#include <string>

namespace server {
namespace lwm2m { class EndpointRegistry; }

namespace web {

struct ProxyTarget {
    std::string target_url;  // http://10.9.0.12:80/
};

class DeviceProxy {
public:
    explicit DeviceProxy(lwm2m::EndpointRegistry& reg);

    /// Resolve a URL path.  The first segment is the endpoint name.
    /// "/urn:dev:gateway-1/" → http://10.9.0.12:80/
    /// "/urn:dev:gateway-1/dashboard" → http://10.9.0.12:80/dashboard
    std::optional<ProxyTarget> resolve(const std::string& path);

private:
    lwm2m::EndpointRegistry& m_reg;
};

} // namespace web
} // namespace server
#endif
