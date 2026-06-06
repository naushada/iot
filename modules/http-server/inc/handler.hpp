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
                      SessionStore* auth);

/// Install /api/v1/cloud/* handlers (L21/D7).  Pointers must outlive
/// the router.  Call after install_handlers().
void install_cloud_handlers(Router& router,
                            server::lwm2m::EndpointRegistry* ep_reg,
                            server::lwm2m::BootstrapProvisioner* provisioner,
                            data_store::Client* ds = nullptr);

} // namespace http_server

#endif
