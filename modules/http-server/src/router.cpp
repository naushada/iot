#include "http_server/router.hpp"

#include <sstream>

namespace http_server {

std::string HttpResponse::to_string() const {
    const char* reason = "OK";
    switch (status) {
        case 200: reason = "OK"; break;
        case 400: reason = "Bad Request"; break;
        case 401: reason = "Unauthorized"; break;
        case 404: reason = "Not Found"; break;
        case 405: reason = "Method Not Allowed"; break;
        case 411: reason = "Length Required"; break;
        case 500: reason = "Internal Server Error"; break;
        default:  reason = "Unknown"; break;
    }
    std::ostringstream ss;
    ss << "HTTP/1.1 " << status << " " << reason << "\r\n"
       << "Content-Type: " << content_type << "\r\n"
       << "Content-Length: " << body.size() << "\r\n";
    for (const auto& h : headers) {
        ss << h.first << ": " << h.second << "\r\n";
    }
    ss << "Connection: close\r\n"
       << "\r\n"
       << body;
    return ss.str();
}

void Router::add(std::string method, std::string path, HandlerFn fn) {
    m_routes[{std::move(method), std::move(path)}] = std::move(fn);
}

HttpResponse Router::route(const HttpParser::Request& req) const {
    auto it = m_routes.find({req.method, req.path});
    if (it != m_routes.end()) {
        return it->second(req);
    }
    // Check if the path exists with a different method → 405
    for (const auto& kv : m_routes) {
        if (kv.first.second == req.path) {
            HttpResponse r;
            r.status = 405;
            r.body = R"({"ok":false,"err":"method not allowed"})";
            return r;
        }
    }
    HttpResponse r;
    r.status = 404;
    r.body = R"({"ok":false,"err":"not found"})";
    return r;
}

} // namespace http_server
