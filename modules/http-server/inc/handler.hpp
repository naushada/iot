#ifndef __http_server_handler_hpp__
#define __http_server_handler_hpp__

#include "router.hpp"

namespace data_store { class Client; }
namespace http_server { class SessionStore; }
namespace server { namespace lwm2m {
    class EndpointRegistry;
    class BootstrapProvisioner;
} }

namespace http_server {

/// Install /api/v1/* handlers on the router.  Pointers must outlive the
/// router (typically owned by main).  `auth` may be nullptr to skip auth
/// wrapping (e.g. for tests), though the /api/v1/auth/* endpoints still
/// need the store for login/logout.
void install_handlers(Router& router,
                      data_store::Client* ds,
                      SessionStore* auth,
                      const std::string& firmware_dir = "");

/// Install /api/v1/cloud/* handlers (L21/D7).  Pointers must outlive
/// the router.  Call after install_handlers().
/// `auth` (optional) scopes reads to the caller's tenant: a session whose
/// tenant is "*" (platform operator) sees all; otherwise only endpoints in the
/// session's tenant. nullptr (or no session) → no filtering (legacy behaviour),
/// so existing single-tenant deployments are unaffected.
void install_cloud_handlers(Router& router,
                            server::lwm2m::EndpointRegistry* ep_reg,
                            server::lwm2m::BootstrapProvisioner* provisioner,
                            SessionStore* auth = nullptr);

/// Install the per-device UI reverse proxy at /dev/<ep>/ (design:
/// apps/docs/tdd-device-ui-path-proxy.md). Resolves <ep> -> dev_tun_ip from
/// cloud.endpoints and proxies over the VPN tun, gated behind the cloud
/// session. Cloud-only effect (a device has no cloud.endpoints → 502).
/// Pointers must outlive the router.
void install_proxy_handler(Router& router,
                           data_store::Client* ds,
                           SessionStore* auth);

} // namespace http_server

#endif
