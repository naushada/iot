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

    /// Serialise to a full HTTP/1.1 response string.
    std::string to_string() const;
};

class Router {
public:
    using HandlerFn = std::function<HttpResponse(
        const HttpParser::Request&)>;

    /// Register a handler for (method, path).
    void add(std::string method, std::string path, HandlerFn fn);

    /// Dispatch a request. Returns 404 if no handler matches.
    HttpResponse route(const HttpParser::Request& req) const;

private:
    using Key = std::pair<std::string, std::string>;  // (method, path)
    std::map<Key, HandlerFn> m_routes;
};

} // namespace http_server

#endif
