/// L21/D7 — Cloud API handler: /api/v1/cloud/* endpoints.

#include "handler.hpp"

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

} // namespace

void install_cloud_handlers(Router& router,
                            server::lwm2m::EndpointRegistry* ep_reg,
                            server::lwm2m::BootstrapProvisioner* provisioner) {

    // ── GET /api/v1/cloud/endpoints ──────────────────────────────
    router.add("GET", "/api/v1/cloud/endpoints",
        [ep_reg](const HttpParser::Request& /*req*/) -> HttpResponse {
            HttpResponse r;
            if (!ep_reg) {
                r.status = 500;
                r.body = R"({"ok":false,"err":"registry not connected"})";
                return r;
            }
            auto eps = ep_reg->list_all();
            json arr = json::array();
            for (const auto& e : eps) {
                json item;
                item["endpoint"]   = e.ep;
                item["tun_ip"]     = e.tun_ip;
                item["proxy_port"] = e.proxy_port;
                item["registered"] = e.registered;
                arr.push_back(item);
            }
            json resp;
            resp["ok"]        = true;
            resp["count"]     = eps.size();
            resp["endpoints"] = arr;
            r.body = resp.dump();
            return r;
        });

    // ── GET /api/v1/cloud/endpoint?ep=<ep> ───────────────────────
    router.add("GET", "/api/v1/cloud/endpoint",
        [ep_reg](const HttpParser::Request& req) -> HttpResponse {
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
            if (!info) {
                r.status = 404;
                r.body = R"({"ok":false,"err":"endpoint not found"})";
                return r;
            }
            json resp;
            resp["ok"]         = true;
            resp["endpoint"]   = info->ep;
            resp["tun_ip"]     = info->tun_ip;
            resp["proxy_port"] = info->proxy_port;
            resp["registered"] = info->registered;
            r.body = resp.dump();
            return r;
        });

    // ── POST /api/v1/cloud/provision ─────────────────────────────
    router.add("POST", "/api/v1/cloud/provision",
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
                r.body = R"({"ok":false,"err":"provision failed (duplicate or exhausted)"})";
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

    // ── POST /api/v1/cloud/deprovision ───────────────────────────
    router.add("POST", "/api/v1/cloud/deprovision",
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
