#include "router.hpp"

#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace http_server {

namespace {

// MIME type for common static extensions
const char* mime_type(const std::string& path) {
    auto ends_with = [&](const char* ext) {
        std::string_view sv{ext};
        auto pos = path.rfind(sv);
        return pos != std::string::npos && pos + sv.size() == path.size();
    };
    if (ends_with(".html")) return "text/html";
    if (ends_with(".js"))   return "application/javascript";
    if (ends_with(".css"))  return "text/css";
    if (ends_with(".svg"))  return "image/svg+xml";
    if (ends_with(".png"))  return "image/png";
    if (ends_with(".ico"))  return "image/x-icon";
    if (ends_with(".json")) return "application/json";
    if (ends_with(".woff2")) return "font/woff2";
    return "application/octet-stream";
}

bool file_exists(const std::string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Read a file into a string.  Returns empty on error.
std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

std::string HttpResponse::to_string(bool keep_alive) const {
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
    ss << "Connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n"
       << "\r\n"
       << body;
    return ss.str();
}

void Router::add(std::string method, std::string path, HandlerFn fn) {
    m_routes[{std::move(method), std::move(path)}] = std::move(fn);
}

void Router::set_static_dir(std::string dir) {
    m_static_dir = std::move(dir);
}

HttpResponse Router::route(const HttpParser::Request& req) const {
    auto it = m_routes.find({req.method, req.path});
    if (it != m_routes.end()) {
        return it->second(req);
    }
    // 405 for known path, wrong method
    for (const auto& kv : m_routes) {
        if (kv.first.second == req.path) {
            HttpResponse r;
            r.status = 405;
            r.body = R"({"ok":false,"err":"method not allowed"})";
            return r;
        }
    }

    // ── Static file serving ───────────────────────────────────
    if (!m_static_dir.empty() && req.method == "GET") {
        std::string file_path = m_static_dir + req.path;

        // SPA fallback: paths without a file extension serve index.html
        bool has_ext = req.path.find('.', req.path.rfind('/') + 1) != std::string::npos;
        if (!has_ext || !file_exists(file_path)) {
            file_path = m_static_dir + "/index.html";
        }

        if (file_exists(file_path)) {
            HttpResponse r;
            r.status = 200;
            r.content_type = mime_type(file_path);
            r.body = read_file(file_path);
            return r;
        }
    }

    HttpResponse r;
    r.status = 404;
    r.body = R"({"ok":false,"err":"not found"})";
    return r;
}

} // namespace http_server
