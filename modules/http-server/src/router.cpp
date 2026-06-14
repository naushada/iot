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
        case 301: reason = "Moved Permanently"; break;
        case 302: reason = "Found"; break;
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

void Router::set_firmware_dir(std::string url_prefix, std::string dir) {
    m_fw_prefix = std::move(url_prefix);
    m_fw_dir = std::move(dir);
}

void Router::set_https_redirect(int https_port) {
    m_https_redirect = https_port;
}

void Router::set_proxy(std::string url_prefix, HandlerFn fn) {
    m_proxy_prefix = std::move(url_prefix);
    m_proxy_fn = std::move(fn);
}

HttpResponse Router::route(const HttpParser::Request& req) const {
    // HTTPS-redirect mode: bounce every request to the https listener.
    // Runs before all other routing so a plain-http :80 instance is a pure
    // redirector to https://<Host>:<port><path>.
    if (m_https_redirect > 0) {
        std::string host;
        auto h = req.headers.find("host");
        if (h != req.headers.end()) host = h->second;
        auto colon = host.rfind(':');           // strip any :port from Host
        if (colon != std::string::npos) host = host.substr(0, colon);
        if (host.empty()) host = "localhost";
        std::string loc = "https://" + host;
        if (m_https_redirect != 443) loc += ":" + std::to_string(m_https_redirect);
        loc += req.path;
        HttpResponse r;
        r.status = 301;
        r.content_type = "text/plain";
        r.headers["Location"] = loc;
        return r;
    }

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

    // ── Per-device UI reverse proxy (/dev/<ep>/...) ────────────
    // Prefix-matched, all methods, after exact routes and before static.
    if (m_proxy_fn && !m_proxy_prefix.empty() &&
        req.path.rfind(m_proxy_prefix, 0) == 0) {
        return m_proxy_fn(req);
    }

    // ── Firmware feed (OTA .ipk) ───────────────────────────────
    // Served under a URL prefix with NO SPA fallback: a missing file is a
    // real 404, and any '..' in the path is rejected (no traversal).
    if (!m_fw_dir.empty() && !m_fw_prefix.empty() && req.method == "GET" &&
        req.path.rfind(m_fw_prefix, 0) == 0) {
        if (req.path.find("..") != std::string::npos) {
            HttpResponse r;
            r.status = 400;
            r.body = R"({"ok":false,"err":"bad path"})";
            return r;
        }
        std::string rel = req.path.substr(m_fw_prefix.size());
        std::string fpath = m_fw_dir;
        if (!fpath.empty() && fpath.back() != '/') fpath += '/';
        fpath += rel;
        HttpResponse r;
        if (file_exists(fpath)) {
            r.status = 200;
            r.content_type = mime_type(fpath);
            r.body = read_file(fpath);
        } else {
            r.status = 404;
            r.body = R"({"ok":false,"err":"firmware not found"})";
        }
        return r;
    }

    // ── Static file serving ───────────────────────────────────
    if (!m_static_dir.empty() && req.method == "GET") {
        std::string file_path = m_static_dir + req.path;

        // SPA fallback: paths without a file extension → serve the
        // nearest index.html (walking up from the requested path).
        // e.g. /webui/dashboard → /webui/index.html
        bool has_ext = req.path.find('.', req.path.rfind('/') + 1) != std::string::npos;
        if (!has_ext || !file_exists(file_path)) {
            std::string dir = req.path;
            bool found = false;
            while (true) {
                std::string candidate = m_static_dir + dir;
                if (!candidate.empty() && candidate.back() != '/')
                    candidate += '/';
                candidate += "index.html";
                if (file_exists(candidate)) {
                    file_path = candidate;
                    found = true;
                    break;
                }
                // Stop once the root ("/") has been checked — otherwise the
                // walk-up below would substr "/" back to "/" forever, hanging
                // the handler thread (a request to "/" with no root index.html
                // is exactly that case).
                if (dir == "/" || dir.empty()) break;
                // Walk up one directory level.
                auto pos = dir.rfind('/');
                if (pos == std::string::npos) break;
                dir = (pos == 0) ? std::string("/") : dir.substr(0, pos);
            }
            if (!found) {
                file_path = m_static_dir + "/index.html";
            }
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
