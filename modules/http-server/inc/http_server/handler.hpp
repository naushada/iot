#ifndef __http_server_handler_hpp__
#define __http_server_handler_hpp__

#include "router.hpp"

#include <memory>

namespace data_store { class Client; }

namespace http_server {

/// Install /api/v1/db/* handlers on the router. The data_store::Client
/// pointer must outlive the router (typically owned by main).
void install_handlers(Router& router, data_store::Client* ds);

} // namespace http_server

#endif
