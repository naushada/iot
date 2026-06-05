#ifndef __http_server_router_hpp__
#define __http_server_router_hpp__

#include "parser.hpp"

#include <functional>
#include <map>
#include <string>
#include <utility>

namespace http_server {

struct HttpResponse {
    int         status = 200;
    std::string content_type = "application/json";
    std::string body;
    /// Extra response headers (e.g. Set-Cookie). Emitted after
    /// Content-* headers in to_string().
    std::map<std::string, std::string> headers;

    /// Serialise to a full HTTP/1.1 response string.  When
    /// `keep_alive` is true the Connection header is "keep-alive";
    /// otherwise it is "close" (the default).
    std::string to_string(bool keep_alive = false) const;
};

class Router {
public:
    using HandlerFn = std::function<HttpResponse(
        const HttpParser::Request&)>;

    /// Register a handler for (method, path).
    void add(std::string method, std::string path, HandlerFn fn);

    /// Set the directory from which static files are served.  When a
    /// request doesn't match any registered handler, the router tries
    /// `static_dir + path`.  SPA fallback: paths without a file
    /// extension return `static_dir/index.html`.  Empty → no static
    /// serving (API-only mode, the default).
    void set_static_dir(std::string dir);

    /// Dispatch a request. Returns 404 if no handler matches.
    HttpResponse route(const HttpParser::Request& req) const;

private:
    using Key = std::pair<std::string, std::string>;  // (method, path)
    std::map<Key, HandlerFn> m_routes;
    std::string m_static_dir;
};

} // namespace http_server

#endif
