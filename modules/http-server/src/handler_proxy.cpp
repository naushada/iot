/// Per-device UI reverse proxy (design: apps/docs/tdd-device-ui-path-proxy.md).
///
/// Serves each device's web UI through the cloud's single origin under
/// /dev/<endpoint>/..., proxied over the VPN tun to dev_tun_ip:<ui_port>.
/// Replaces the per-device published-port + nftables DNAT approach.
///
/// Three rewrites make the device SPA (built with <base href="/">, empty
/// apiUrl, relative <script> tags) work under the prefix with no per-device
/// rebuild:
///   1. request path strip   /dev/<ep>/foo -> GET /foo upstream
///   2. <base href> inject    "/" -> "/dev/<ep>/"  (relative assets resolve back)
///   3. Set-Cookie Path       "/" -> "/dev/<ep>/"  (per-device cookie isolation)
///
/// Gated behind the cloud operator session; resolves <ep> -> dev_tun_ip from
/// cloud.endpoints (SSRF-safe: only known tunnel IPs, fixed upstream port).

#include "handler.hpp"
#include "router.hpp"
#include "auth.hpp"

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include <ace/INET_Addr.h>
#include <ace/Log_Msg.h>
#include <ace/SOCK_Connector.h>
#include <ace/SOCK_Stream.h>
#include <ace/Time_Value.h>

#include <nlohmann/json.hpp>

#include "data_store/client.hpp"
#include "data_store/value.hpp"

namespace http_server {

namespace {

using json = nlohmann::json;

constexpr const char* kPrefix = "/dev/";

// Percent-decode a URL path segment (the encoded endpoint name).
std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() &&
            std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            auto hex = [](char c) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return (c >= 'a') ? (c - 'a' + 10) : (c - '0');
            };
            out.push_back(static_cast<char>(hex(s[i + 1]) * 16 + hex(s[i + 2])));
            i += 2;
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::string ds_str(data_store::Client* ds, const std::string& key,
                   const std::string& dflt) {
    std::vector<data_store::Client::GetResult> got;
    if (ds && ds->get({key}, got).ok && !got.empty() && got[0].has_value)
        if (auto s = data_store::to_string(got[0].value)) return *s;
    return dflt;
}

int ds_int(data_store::Client* ds, const std::string& key, int dflt) {
    std::vector<data_store::Client::GetResult> got;
    if (ds && ds->get({key}, got).ok && !got.empty() && got[0].has_value)
        if (auto n = data_store::to_int32(got[0].value)) return *n;
    return dflt;
}

// Resolve a (decoded) endpoint name to its live tunnel IP via cloud.endpoints.
// Returns empty when the endpoint is unknown or has no dev_tun_ip (tunnel down).
std::string resolve_dev_tun_ip(data_store::Client* ds, const std::string& ep) {
    try {
        auto arr = json::parse(ds_str(ds, "cloud.endpoints", "[]"));
        if (arr.is_array()) {
            for (const auto& e : arr) {
                if (!e.is_object()) continue;
                if (e.value("endpoint", std::string()) != ep) continue;
                return e.value("dev_tun_ip", std::string());
            }
        }
    } catch (const std::exception&) {}
    return {};
}

bool is_hop_header(const std::string& k) {
    return k == "host" || k == "connection" || k == "keep-alive" ||
           k == "proxy-connection" || k == "transfer-encoding" ||
           k == "upgrade" || k == "content-length";
}

// Drop the cloud session cookie from a Cookie header before forwarding upstream,
// so the operator's cloud session token never leaves the cloud for the device.
std::string strip_cookie(const std::string& cookie_hdr, const std::string& name) {
    std::string out;
    std::size_t i = 0;
    while (i < cookie_hdr.size()) {
        std::size_t semi = cookie_hdr.find(';', i);
        std::string pair = cookie_hdr.substr(i, semi == std::string::npos
                                                 ? std::string::npos : semi - i);
        // trim leading spaces
        std::size_t s = pair.find_first_not_of(' ');
        std::string trimmed = (s == std::string::npos) ? "" : pair.substr(s);
        std::size_t eq = trimmed.find('=');
        std::string key = (eq == std::string::npos) ? trimmed : trimmed.substr(0, eq);
        if (key != name && !trimmed.empty()) {
            if (!out.empty()) out += "; ";
            out += trimmed;
        }
        if (semi == std::string::npos) break;
        i = semi + 1;
    }
    return out;
}

// Open a connection to dev_tun_ip:port, send `request`, read the full response
// until EOF (we send Connection: close). Returns false on connect/timeout error.
bool upstream_exchange(const std::string& ip, std::uint16_t port,
                       const std::string& request, std::string& response) {
    ACE_INET_Addr addr(port, ip.c_str());
    ACE_SOCK_Connector conn;
    ACE_SOCK_Stream stream;
    ACE_Time_Value ct(5, 0);   // connect timeout
    if (conn.connect(stream, addr, &ct) != 0) return false;

    if (stream.send_n(request.data(), request.size()) < 0) {
        stream.close();
        return false;
    }
    char buf[8192];
    ACE_Time_Value rt(75, 0);  // recv timeout > the longest device long-poll
    for (;;) {
        ssize_t n = stream.recv(buf, sizeof(buf), &rt);
        if (n > 0) { response.append(buf, static_cast<std::size_t>(n)); continue; }
        break;     // 0 = EOF (peer closed), <0 = timeout/error
    }
    stream.close();
    return !response.empty();
}

HttpResponse simple(int status, const std::string& ctype, const std::string& body) {
    HttpResponse r;
    r.status = status;
    r.content_type = ctype;
    r.body = body;
    return r;
}

HttpResponse redirect(const std::string& location) {
    HttpResponse r;
    r.status = 302;
    r.content_type = "text/plain";
    r.headers["Location"] = location;
    return r;
}

} // namespace

void install_proxy_handler(Router& router, data_store::Client* ds,
                           SessionStore* auth) {
    router.set_proxy(kPrefix, [ds, auth](const HttpParser::Request& req)
                                   -> HttpResponse {
        // ── cloud auth gate ────────────────────────────────────────
        // /dev/* is reachable on the public origin, so require a valid cloud
        // operator session (the browser already holds it from the cloud UI).
        if (auth && auth->enabled()) {
            std::string tok = extract_session_cookie(req.headers,
                                                     auth->cookie_name());
            if (tok.empty() || !auth->validate(tok)) {
                return redirect("/webui/");   // bounce to the cloud login
            }
        }

        // ── split /dev/<epseg>/<tail> from the full request target ─
        // Use the raw (encoded) segment for the browser-facing prefix (base
        // href / cookie Path), and the decoded name for the cloud.endpoints
        // lookup.
        std::string after = req.url.substr(std::string(kPrefix).size());
        std::size_t slash = after.find('/');
        std::string epseg = (slash == std::string::npos) ? after
                                                          : after.substr(0, slash);
        std::string tail  = (slash == std::string::npos) ? "/"
                                                         : after.substr(slash);
        if (tail.empty()) tail = "/";
        if (epseg.empty()) return simple(404, "text/plain", "no endpoint");

        const std::string ep     = url_decode(epseg);
        const std::string prefix = std::string(kPrefix) + epseg + "/";  // /dev/<epseg>/

        // ── resolve endpoint -> live tunnel IP (SSRF-safe) ─────────
        std::string ip = resolve_dev_tun_ip(ds, ep);
        if (ip.empty()) {
            return simple(502, "text/plain",
                          "device tunnel is down (no route to " + ep + ")");
        }
        auto ui_port = static_cast<std::uint16_t>(
            ds_int(ds, "cloud.proxy.device.ui.port", 8080));

        // ── build the upstream request ─────────────────────────────
        std::string out = req.method + " " + tail + " HTTP/1.1\r\n";
        out += "Host: " + ip + "\r\n";
        for (const auto& h : req.headers) {
            if (is_hop_header(h.first)) continue;
            if (h.first == "cookie") {
                std::string c = auth ? strip_cookie(h.second, auth->cookie_name())
                                     : h.second;
                if (!c.empty()) out += "Cookie: " + c + "\r\n";
                continue;
            }
            out += h.first + ": " + h.second + "\r\n";
        }
        out += "Connection: close\r\n";
        if (!req.body.empty())
            out += "Content-Length: " + std::to_string(req.body.size()) + "\r\n";
        out += "\r\n";
        out += req.body;

        // ── exchange ───────────────────────────────────────────────
        std::string raw;
        if (!upstream_exchange(ip, ui_port, out, raw)) {
            return simple(504, "text/plain", "device UI did not respond");
        }

        // ── parse the upstream response ────────────────────────────
        std::size_t hdr_end = raw.find("\r\n\r\n");
        if (hdr_end == std::string::npos) {
            return simple(502, "text/plain", "bad upstream response");
        }
        std::string head = raw.substr(0, hdr_end);
        std::string body = raw.substr(hdr_end + 4);

        // status line
        int status = 200;
        std::size_t nl = head.find("\r\n");
        std::string status_line = head.substr(0, nl);
        {
            std::size_t sp = status_line.find(' ');
            if (sp != std::string::npos)
                status = std::atoi(status_line.c_str() + sp + 1);
        }

        HttpResponse r;
        r.status = status;
        r.content_type = "application/octet-stream";

        // headers
        std::size_t pos = (nl == std::string::npos) ? head.size() : nl + 2;
        bool is_html = false;
        while (pos < head.size()) {
            std::size_t eol = head.find("\r\n", pos);
            std::string line = head.substr(pos, eol == std::string::npos
                                                ? std::string::npos : eol - pos);
            pos = (eol == std::string::npos) ? head.size() : eol + 2;
            std::size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            if (!val.empty() && val[0] == ' ') val.erase(0, 1);
            std::string lkey = key;
            for (auto& c : lkey) c = static_cast<char>(
                std::tolower(static_cast<unsigned char>(c)));

            if (lkey == "content-length" || lkey == "connection" ||
                lkey == "transfer-encoding" || lkey == "keep-alive") {
                continue;   // recomputed by HttpResponse::to_string
            }
            if (lkey == "content-type") {
                r.content_type = val;
                if (val.find("text/html") != std::string::npos) is_html = true;
                continue;
            }
            if (lkey == "set-cookie") {
                // Rewrite Path so the device cookie is scoped to /dev/<ep>/.
                std::string nv = val;
                std::size_t pp = nv.find("Path=/");
                if (pp != std::string::npos) {
                    // replace the Path value up to the next ';' with our prefix
                    std::size_t pe = nv.find(';', pp);
                    std::string newpath = "Path=" + prefix;
                    nv = nv.substr(0, pp) + newpath +
                         (pe == std::string::npos ? "" : nv.substr(pe));
                } else {
                    nv += "; Path=" + prefix;
                }
                r.headers["Set-Cookie"] = nv;
                continue;
            }
            if (lkey == "location" && !val.empty() && val[0] == '/' &&
                val.rfind(std::string(kPrefix), 0) != 0) {
                // prefix server-relative redirects
                r.headers["Location"] = "/dev/" + epseg + val;
                continue;
            }
            r.headers[key] = val;
        }

        // ── base-href rewrite for the SPA index ────────────────────
        if (is_html) {
            const char* needles[] = { "<base href=\"/\">", "<base href='/'>" };
            for (const char* n : needles) {
                std::size_t bp = body.find(n);
                if (bp != std::string::npos) {
                    std::string rep = "<base href=\"" + prefix + "\">";
                    body.replace(bp, std::string(n).size(), rep);
                    break;
                }
            }
        }
        r.body = std::move(body);
        return r;
    });

    ACE_DEBUG((LM_INFO, ACE_TEXT("%D httpd:thread:%t %M %N:%l device-UI reverse "
                                 "proxy installed at %C<ep>/\n"), kPrefix));
}

} // namespace http_server
