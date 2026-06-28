/// L21/D7 — Cloud API handler: /api/v1/cloud/* endpoints.

#include "handler.hpp"

#include "auth.hpp"
#include "endpoint_registry.hpp"
#include "bootstrap.hpp"

#include <nlohmann/json.hpp>
#include <string>

namespace http_server {

namespace {

using json = nlohmann::json;

json parse_body(const std::string& body) {
    if (body.empty()) return json::object();
    try { return json::parse(body); }
    catch (const std::exception&) { return json::object(); }
}

/// The tenant this request acts as. No auth wired → "*" (no filtering, legacy).
/// Authenticated → the session's tenant; no/invalid session → "default".
std::string request_tenant(const HttpParser::Request& req, SessionStore* auth) {
    if (!auth) return "*";
    const std::string token =
        extract_session_cookie(req.headers, auth->cookie_name());
    const auto* s = token.empty() ? nullptr : auth->validate(token);
    return s ? s->tenant : std::string("default");
}

/// An endpoint row (its tenant, "" == "default") is visible to a viewer whose
/// tenant is `as` when `as` is "*" or the tenants match.
bool visible_to(const std::string& row_tenant, const std::string& as) {
    if (as == "*") return true;
    const std::string rt = row_tenant.empty() ? std::string("default") : row_tenant;
    return rt == as;
}

} // namespace

void install_cloud_handlers(Router& router,
                            server::lwm2m::EndpointRegistry* ep_reg,
                            server::lwm2m::BootstrapProvisioner* provisioner,
                            SessionStore* auth) {

    // ── GET /api/v1/cloud/endpoints ──────────────────────────────
    router.add("GET", "/api/v1/cloud/endpoints",
        [ep_reg, auth](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            if (!ep_reg) {
                r.status = 500;
                r.body = R"({"ok":false,"err":"registry not connected"})";
                return r;
            }
            const std::string as = request_tenant(req, auth);
            auto eps = ep_reg->list_all();
            json arr = json::array();
            for (const auto& e : eps) {
                if (!visible_to(e.tenant, as)) continue;   // tenant read-isolation
                json item;
                item["endpoint"]   = e.ep;
                item["tun_ip"]     = e.tun_ip;
                item["dev_tun_ip"] = e.dev_tun_ip;
                item["proxy_port"] = e.proxy_port;
                item["registered"] = e.registered;
                item["tenant"]     = e.tenant.empty() ? "default" : e.tenant;
                arr.push_back(item);
            }
            json resp;
            resp["ok"]        = true;
            resp["count"]     = arr.size();   // visible (tenant-scoped) count
            resp["endpoints"] = arr;
            r.body = resp.dump();
            return r;
        });

    // ── GET /api/v1/cloud/endpoint?ep=<ep> ───────────────────────
    router.add("GET", "/api/v1/cloud/endpoint",
        [ep_reg, auth](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            if (!ep_reg) {
                r.status = 500;
                r.body = R"({"ok":false,"err":"registry not connected"})";
                return r;
            }
            auto it = req.query.find("ep");
            if (it == req.query.end()) {
                r.status = 400;
                r.body = R"({"ok":false,"err":"missing 'ep' query param"})";
                return r;
            }
            std::string ep = it->second;
            const auto* info = ep_reg->lookup_by_ep(ep);
            // Out-of-tenant endpoints are indistinguishable from missing ones
            // (don't leak existence across tenants).
            if (!info || !visible_to(info->tenant, request_tenant(req, auth))) {
                r.status = 404;
                r.body = R"({"ok":false,"err":"endpoint not found"})";
                return r;
            }
            json resp;
            resp["ok"]         = true;
            resp["endpoint"]   = info->ep;
            resp["tun_ip"]     = info->tun_ip;
            resp["dev_tun_ip"] = info->dev_tun_ip;
            resp["proxy_port"] = info->proxy_port;
            resp["registered"] = info->registered;
            r.body = resp.dump();
            return r;
        });

    // ── POST /api/v1/cloud/endpoints (provision) ─────────────────
    router.add("POST", "/api/v1/cloud/endpoints",
        [provisioner](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            if (!provisioner) {
                r.status = 500;
                r.body = R"({"ok":false,"err":"provisioner not available"})";
                return r;
            }
            auto doc = parse_body(req.body);
            std::string ep = doc.value("endpoint", "");
            if (ep.empty()) {
                r.status = 400;
                r.body = R"({"ok":false,"err":"missing 'endpoint' field"})";
                return r;
            }
            auto result = provisioner->provision(ep);
            if (!result.has_value()) {
                r.status = 400;
                r.body = R"json({"ok":false,"err":"provision failed - duplicate or exhausted"})json";
                return r;
            }
            json resp;
            resp["ok"]         = true;
            resp["endpoint"]   = result->endpoint;
            resp["tun_ip"]     = result->tun_ip;
            resp["proxy_port"] = result->proxy_port;
            r.body = resp.dump();
            return r;
        });

    // ── DELETE /api/v1/cloud/endpoints (deprovision) ────────────
    router.add("DELETE", "/api/v1/cloud/endpoints",
        [provisioner](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            if (!provisioner) {
                r.status = 500;
                r.body = R"({"ok":false,"err":"provisioner not available"})";
                return r;
            }
            auto it = req.query.find("ep");
            if (it == req.query.end()) {
                r.status = 400;
                r.body = R"({"ok":false,"err":"missing 'ep' query param"})";
                return r;
            }
            std::string ep = it->second;
            bool ok = provisioner->deprovision(ep);
            json resp;
            resp["ok"]       = ok;
            resp["endpoint"] = ep;
            if (!ok) resp["err"] = "endpoint not found";
            r.body = resp.dump();
            if (!ok) r.status = 404;
            return r;
        });

}

} // namespace http_server
