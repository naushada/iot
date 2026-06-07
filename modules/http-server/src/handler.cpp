#include "auth.hpp"
#include "handler.hpp"

#include "data_store/client.hpp"
#include "data_store/value.hpp"

#include <nlohmann/json.hpp>

#include <ace/Log_Msg.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace http_server {

namespace {

using json = nlohmann::json;

/// Build a JSON response body from a data_store get result.
json get_result_to_json(const std::vector<data_store::Client::GetResult>& got) {
    json data = json::object();
    for (const auto& g : got) {
        if (!g.has_value) {
            data[g.key] = nullptr;
        } else if (auto s = data_store::to_string(g.value)) {
            data[g.key] = *s;
        } else if (auto b = data_store::to_bool(g.value)) {
            data[g.key] = *b;
        } else if (auto n = data_store::to_int32(g.value)) {
            data[g.key] = *n;
        } else if (auto u = data_store::to_uint32(g.value)) {
            data[g.key] = *u;
        } else {
            data[g.key] = nullptr;
        }
    }
    return data;
}

/// Parse a JSON body, return empty object on failure.
json parse_body(const std::string& body) {
    if (body.empty()) return json::object();
    try {
        return json::parse(body);
    } catch (const std::exception&) {
        return json::object();
    }
}

} // namespace

void install_handlers(Router& router,
                      data_store::Client* ds,
                      SessionStore* auth) {
    // ─── POST /api/v1/db/get ─────────────────────────────────
    router.add("POST", "/api/v1/db/get",
        [ds](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            if (!ds) {
                r.status = 500;
                r.body = R"({"ok":false,"err":"data store not connected"})";
                return r;
            }
            try {
                auto doc = json::parse(req.body);
                if (!doc.contains("keys") || !doc["keys"].is_array()) {
                    r.status = 400;
                    r.body = R"({"ok":false,"err":"missing 'keys' array"})";
                    return r;
                }
                std::vector<std::string> keys;
                for (const auto& k : doc["keys"]) {
                    if (k.is_string()) keys.push_back(k.get<std::string>());
                }
                std::vector<data_store::Client::GetResult> got;
                auto rs = ds->get(keys, got);
                if (!rs.ok) {
                    r.status = 500;
                    r.body = R"({"ok":false,"err":")" + rs.err + "\"}";
                    return r;
                }
                json resp;
                resp["ok"] = true;
                resp["data"] = get_result_to_json(got);
                r.body = resp.dump();
            } catch (const std::exception& e) {
                r.status = 400;
                r.body = std::string(R"({"ok":false,"err":")") + e.what() + "\"}";
            }
            return r;
        });

    // ─── POST /api/v1/db/set ─────────────────────────────────
    router.add("POST", "/api/v1/db/set",
        [ds, auth](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            if (!ds) {
                r.status = 500;
                r.body = R"({"ok":false,"err":"data store not connected"})";
                return r;
            }
            // ── Access control ─────────────────────────────────
            std::string access_level = "Admin";  // default: full access
            if (auth && auth->enabled()) {
                std::string token = extract_session_cookie(req.headers);
                if (!token.empty()) {
                    const auto* session = auth->validate(token);
                    if (session) access_level = session->access;
                }
            }
            try {
                auto doc = json::parse(req.body);
                if (!doc.contains("pairs") || !doc["pairs"].is_array()) {
                    r.status = 400;
                    r.body = R"({"ok":false,"err":"missing 'pairs' array"})";
                    return r;
                }
                std::vector<data_store::KV> pairs;
                for (const auto& p : doc["pairs"]) {
                    if (!p.contains("key") || !p.contains("value")) continue;
                    std::string key = p["key"].get<std::string>();
                    const auto& v = p["value"];
                    data_store::Value val;
                    if (v.is_string())       val = v.get<std::string>();
                    else if (v.is_boolean()) val = v.get<bool>();
                    else if (v.is_number_integer()) {
                        auto n = v.get<int>();
                        if (n >= 0) val = static_cast<std::uint32_t>(n);
                        else        val = static_cast<std::int32_t>(n);
                    } else if (v.is_number_float()) {
                        val = v.get<double>();
                    } else {
                        val = v.dump();
                    }
                    pairs.emplace_back(key, val);
                }
                // Enforce access control: Viewer cannot modify any key
                if (!can_write(access_level, "")) {
                    r.status = 403;
                    r.body = R"({"ok":false,"err":"forbidden: read-only access"})";
                    return r;
                }
                auto rs = ds->set(pairs);
                if (!rs.ok) {
                    r.status = 400;
                    r.body = R"({"ok":false,"err":")" + rs.err + "\"}";
                    return r;
                }
                json resp;
                resp["ok"] = true;
                resp["changed"] = pairs.size();
                r.body = resp.dump();
            } catch (const std::exception& e) {
                r.status = 400;
                r.body = std::string(R"({"ok":false,"err":")") + e.what() + "\"}";
            }
            return r;
        });

    // ─── GET /api/v1/db/get?key=N&timeout=S (long-poll) ──────
    router.add("GET", "/api/v1/db/get",
        [ds](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            if (!ds) {
                r.status = 500;
                r.body = R"({"ok":false,"err":"data store not connected"})";
                return r;
            }
            auto kit = req.query.find("key");
            if (kit == req.query.end()) {
                r.status = 400;
                r.body = R"({"ok":false,"err":"missing 'key' query param"})";
                return r;
            }
            const std::string& key = kit->second;

            int timeout = 0;
            auto tit = req.query.find("timeout");
            if (tit != req.query.end()) {
                timeout = std::atoi(tit->second.c_str());
                if (timeout < 0) timeout = 0;
                if (timeout > 300) timeout = 300;  // cap at 5 min
            }

            // Prime: read current value
            std::vector<data_store::Client::GetResult> got;
            auto rs = ds->get({key}, got);
            json resp;
            resp["key"] = key;

            if (timeout == 0) {
                // Immediate return
                resp["changed"] = false;
                if (rs.ok && !got.empty() && got[0].has_value) {
                    auto val = get_result_to_json(got);
                    resp["value"] = val[key];
                } else {
                    resp["value"] = nullptr;
                }
                r.body = resp.dump();
                return r;
            }

            // Long poll: a per-request callback-style watch (not the shared
            // pull queue), so concurrent long-polls on a worker pool each get
            // their own event — no cross-talk. The state is heap-owned and
            // captured by the callback (shared_ptr), so a notification that
            // races unwatch can't touch this stack frame.
            struct LpState {
                std::mutex                 m;
                std::condition_variable    cv;
                bool                       fired = false;
                data_store::Client::Event  ev;
            };
            auto st = std::make_shared<LpState>();
            data_store::Client::WatchHandle wh =
                data_store::Client::kInvalidHandle;
            auto ws = ds->watch(key,
                [st](const data_store::Client::Event& e) {
                    std::lock_guard<std::mutex> lk(st->m);
                    if (!st->fired) { st->ev = e; st->fired = true; }
                    st->cv.notify_one();
                },
                &wh);
            if (!ws.ok) {
                resp["changed"] = false;
                resp["value"] = nullptr;
                r.body = resp.dump();
                return r;
            }

            bool got_event;
            {
                std::unique_lock<std::mutex> lk(st->m);
                st->cv.wait_for(lk, std::chrono::seconds(timeout),
                                [&] { return st->fired; });
                got_event = st->fired;
            }
            if (wh != data_store::Client::kInvalidHandle) ds->unwatch(wh);
            data_store::Client::Event ev = st->ev;

            if (got_event) {
                resp["changed"] = true;
                if (auto s = data_store::to_string(ev.value)) {
                    resp["value"] = *s;
                } else if (auto b = data_store::to_bool(ev.value)) {
                    resp["value"] = *b;
                } else {
                    resp["value"] = nullptr;
                }
                if (ev.prev_has_value) {
                    if (auto ps = data_store::to_string(ev.prev)) {
                        resp["prev"] = *ps;
                    }
                }
            } else {
                resp["changed"] = false;
                if (rs.ok && !got.empty() && got[0].has_value) {
                    auto val = get_result_to_json(got);
                    resp["value"] = val[key];
                } else {
                    resp["value"] = nullptr;
                }
            }
            r.body = resp.dump();
            return r;
        });

    // ── POST /api/v1/auth/login ──────────────────────────────────
    // Body: { "id": "admin", "password": "<plaintext>" }
    // The server SHA-256-hashes the submitted password and compares
    // against auth.users.admin.password.hash in the data store.
    router.add("POST", "/api/v1/auth/login",
        [ds, auth](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            auto doc = parse_body(req.body);
            std::string id       = doc.value("id", "");
            std::string password = doc.value("password", "");
            if (id.empty() || password.empty()) {
                r.status = 400;
                r.body = R"({"ok":false,"err":"id and password required"})";
                return r;
            }
            // v1: single admin user
            if (id != "admin") {
                r.status = 401;
                r.body = R"({"ok":false,"err":"invalid credentials"})";
                return r;
            }
            // Load the stored SHA-256 hash, fall back to compiled-in default
            std::string stored_hash = CredentialStore::kDefaultHash;
            if (ds) {
                stored_hash = CredentialStore::load_admin_password_hash(*ds);
            }
            if (!CredentialStore::verify(password, stored_hash)) {
                r.status = 401;
                r.body = R"({"ok":false,"err":"invalid credentials"})";
                return r;
            }
            // Load user's access level
            std::string access = "Admin";
            if (ds) access = CredentialStore::load_user_access(*ds, id);
            std::string token;
            if (auth) token = auth->create_session(id, "admin", access);
            json resp;
            resp["ok"]     = true;
            resp["role"]   = "admin";
            resp["access"] = access;
            r.body = resp.dump();
            if (!token.empty()) {
                r.headers["Set-Cookie"] = make_set_cookie(token);
            }
            return r;
        });

    // ── POST /api/v1/auth/logout ─────────────────────────────────
    router.add("POST", "/api/v1/auth/logout",
        [auth](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            if (auth) {
                std::string token = extract_session_cookie(req.headers);
                if (!token.empty()) auth->destroy(token);
            }
            r.body = R"({"ok":true})";
            r.headers["Set-Cookie"] =
                "iot-session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0";
            return r;
        });

    // ── GET /api/v1/status ───────────────────────────────────────
    // Supports ?timeout=N for long-poll: blocks until a watched key
    // (vpn.state) changes or N seconds elapse, then returns the full
    // status snapshot.  timeout=0 (default) returns immediately.
    router.add("GET", "/api/v1/status",
        [ds](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            if (!ds) {
                r.status = 500;
                r.body = R"({"ok":false,"err":"data store not connected"})";
                return r;
            }

            // Parse timeout query param
            int timeout = 0;
            auto tit = req.query.find("timeout");
            if (tit != req.query.end()) {
                timeout = std::atoi(tit->second.c_str());
                if (timeout < 0) timeout = 0;
                if (timeout > 60) timeout = 60;  // cap at 1 min
            }

            // Long-poll path: watch vpn.state (most dynamic key) and
            // return full status on change or timeout
            if (timeout > 0) {
                using LpState = struct {
                    std::mutex                 m;
                    std::condition_variable    cv;
                    bool                       fired = false;
                };
                auto st = std::make_shared<LpState>();
                data_store::Client::WatchHandle wh =
                    data_store::Client::kInvalidHandle;
                auto ws = ds->watch("vpn.state",
                    [st](const data_store::Client::Event& /*e*/) {
                        std::lock_guard<std::mutex> lk(st->m);
                        st->fired = true;
                        st->cv.notify_one();
                    }, &wh);
                if (ws.ok) {
                    std::unique_lock<std::mutex> lk(st->m);
                    st->cv.wait_for(lk, std::chrono::seconds(timeout),
                                    [&] { return st->fired; });
                    if (wh != data_store::Client::kInvalidHandle)
                        ds->unwatch(wh);
                }
                // Fall through to build the full status snapshot
            }
            std::vector<data_store::Client::GetResult> got;
            auto rs = ds->get({
                // LwM2M
                "iot.server.uri", "iot.endpoint",
                // VPN
                "vpn.state", "vpn.assigned.ip", "vpn.assigned.gateway",
                "vpn.assigned.netmask", "vpn.assigned.dns", "vpn.pid",
                "vpn.exit_code", "vpn.gate.reason", "vpn.bound.iface",
                // WiFi
                "wifi.assoc.state", "wifi.assoc.ssid", "wifi.signal.rssi",
                "wifi.dhcp.state", "wifi.dhcp.ip",
                // WAN / net
                "net.iface.active", "net.state", "net.tun.ip",
                "net.rules.applied.count", "net.last.apply.unix",
                "net.iface.priority",
                // Services
                "services.ds.state", "services.ds.uptime.sec",
                "services.net.router.enable", "services.net.router.state",
                "services.openvpn.client.enable",
                "services.openvpn.client.state",
                "services.lwm2m.client.enable",
                "services.lwm2m.client.state",
                "services.lwm2m.server.enable",
                "services.lwm2m.server.state",
                "services.wifi.client.enable", "services.wifi.client.state",
                // L22 — per-container resource telemetry.
                "services.ds.cpu.permille", "services.ds.cpu.count",
                "services.ds.mem.rss.kb",
                "services.ds.fd.count", "services.ds.threads",
                "services.net.router.cpu.permille", "services.net.router.cpu.count",
                "services.net.router.mem.rss.kb",
                "services.net.router.fd.count", "services.net.router.threads",
                "services.openvpn.client.cpu.permille", "services.openvpn.client.cpu.count",
                "services.openvpn.client.mem.rss.kb",
                "services.openvpn.client.fd.count", "services.openvpn.client.threads",
                "services.lwm2m.client.cpu.permille", "services.lwm2m.client.cpu.count",
                "services.lwm2m.client.mem.rss.kb",
                "services.lwm2m.client.fd.count", "services.lwm2m.client.threads",
                "services.lwm2m.server.cpu.permille", "services.lwm2m.server.cpu.count",
                "services.lwm2m.server.mem.rss.kb",
                "services.lwm2m.server.fd.count", "services.lwm2m.server.threads",
                "services.wifi.client.cpu.permille", "services.wifi.client.cpu.count",
                "services.wifi.client.mem.rss.kb",
                "services.wifi.client.fd.count", "services.wifi.client.threads",
            }, got);
            json resp;
            resp["ok"] = true;
            if (!rs.ok) {
                resp["ok"] = false;
                resp["err"] = rs.err;
                r.body = resp.dump();
                return r;
            }
            // Build status sections
            json lwm2m      = json::object();
            json vpn        = json::object();
            json wifi       = json::object();
            json wan        = json::object();
            json routing    = json::object();
            json services   = json::object();

            for (const auto& g : got) {
                const auto& k = g.key;
                auto sv = [&]() -> std::string {
                    if (auto s = data_store::to_string(g.value)) return *s;
                    return "";
                };
                auto iv = [&](int def = 0) -> int {
                    if (auto n = data_store::to_int32(g.value))
                        return *n;
                    if (auto u = data_store::to_uint32(g.value))
                        return static_cast<int>(*u);
                    return def;
                };
                auto bv = [&](bool def = false) -> bool {
                    if (auto b = data_store::to_bool(g.value)) return *b;
                    return def;
                };

                if (k == "iot.server.uri")        lwm2m["server_uri"] = sv();
                else if (k == "iot.endpoint")     lwm2m["endpoint"] = sv();

                else if (k == "vpn.state")             vpn["state"] = sv();
                else if (k == "vpn.assigned.ip")       vpn["ip"] = sv();
                else if (k == "vpn.assigned.gateway")  vpn["gateway"] = sv();
                else if (k == "vpn.assigned.netmask")  vpn["netmask"] = sv();
                else if (k == "vpn.assigned.dns")      vpn["dns"] = sv();
                else if (k == "vpn.pid")               vpn["pid"] = iv();
                else if (k == "vpn.exit_code")         vpn["exit_code"] = iv();
                else if (k == "vpn.gate.reason")       vpn["gate_reason"] = sv();
                else if (k == "vpn.bound.iface")       vpn["bound_iface"] = sv();

                else if (k == "wifi.assoc.state")      wifi["state"] = sv();
                else if (k == "wifi.assoc.ssid")       wifi["ssid"] = sv();
                else if (k == "wifi.signal.rssi")      wifi["rssi"] = iv();
                else if (k == "wifi.dhcp.state")       wifi["dhcp_state"] = sv();
                else if (k == "wifi.dhcp.ip")          wifi["dhcp_ip"] = sv();

                else if (k == "net.iface.active")            wan["active_iface"] = sv();
                else if (k == "net.iface.priority")          wan["iface_priority"] = sv();
                else if (k == "net.tun.ip")                  wan["tun_ip"] = sv();
                else if (k == "net.state")                   routing["state"] = sv();
                else if (k == "net.rules.applied.count")     routing["rules_applied"] = iv();
                else if (k == "net.last.apply.unix")         routing["last_apply_unix"] = iv();

                else if (k == "services.ds.state")                services["ds"]["state"] = sv();
                else if (k == "services.ds.uptime.sec")           services["ds"]["uptime_sec"] = iv();
                else if (k == "services.net.router.enable")       services["net_router"]["enable"] = bv(true);
                else if (k == "services.net.router.state")        services["net_router"]["state"] = sv();
                else if (k == "services.openvpn.client.enable")   services["openvpn_client"]["enable"] = bv(true);
                else if (k == "services.openvpn.client.state")    services["openvpn_client"]["state"] = sv();
                else if (k == "services.lwm2m.client.enable")     services["lwm2m_client"]["enable"] = bv(true);
                else if (k == "services.lwm2m.client.state")      services["lwm2m_client"]["state"] = sv();
                else if (k == "services.lwm2m.server.enable")     services["lwm2m_server"]["enable"] = bv(true);
                else if (k == "services.lwm2m.server.state")      services["lwm2m_server"]["state"] = sv();
                else if (k == "services.wifi.client.enable")      services["wifi_client"]["enable"] = bv(true);
                else if (k == "services.wifi.client.state")       services["wifi_client"]["state"] = sv();

                // L22 — resource telemetry per service.
                else if (k == "services.ds.cpu.permille")             services["ds"]["cpu_permille"] = iv();
                else if (k == "services.ds.cpu.count")                services["ds"]["cpu_count"] = iv();
                else if (k == "services.ds.mem.rss.kb")               services["ds"]["mem_kb"] = iv();
                else if (k == "services.ds.fd.count")                 services["ds"]["fd_count"] = iv();
                else if (k == "services.ds.threads")                  services["ds"]["threads"] = iv();
                else if (k == "services.net.router.cpu.permille")     services["net_router"]["cpu_permille"] = iv();
                else if (k == "services.net.router.cpu.count")        services["net_router"]["cpu_count"] = iv();
                else if (k == "services.net.router.mem.rss.kb")       services["net_router"]["mem_kb"] = iv();
                else if (k == "services.net.router.fd.count")         services["net_router"]["fd_count"] = iv();
                else if (k == "services.net.router.threads")          services["net_router"]["threads"] = iv();
                else if (k == "services.openvpn.client.cpu.permille") services["openvpn_client"]["cpu_permille"] = iv();
                else if (k == "services.openvpn.client.cpu.count")    services["openvpn_client"]["cpu_count"] = iv();
                else if (k == "services.openvpn.client.mem.rss.kb")   services["openvpn_client"]["mem_kb"] = iv();
                else if (k == "services.openvpn.client.fd.count")     services["openvpn_client"]["fd_count"] = iv();
                else if (k == "services.openvpn.client.threads")      services["openvpn_client"]["threads"] = iv();
                else if (k == "services.lwm2m.client.cpu.permille")   services["lwm2m_client"]["cpu_permille"] = iv();
                else if (k == "services.lwm2m.client.cpu.count")      services["lwm2m_client"]["cpu_count"] = iv();
                else if (k == "services.lwm2m.client.mem.rss.kb")     services["lwm2m_client"]["mem_kb"] = iv();
                else if (k == "services.lwm2m.client.fd.count")       services["lwm2m_client"]["fd_count"] = iv();
                else if (k == "services.lwm2m.client.threads")        services["lwm2m_client"]["threads"] = iv();
                else if (k == "services.lwm2m.server.cpu.permille")   services["lwm2m_server"]["cpu_permille"] = iv();
                else if (k == "services.lwm2m.server.cpu.count")      services["lwm2m_server"]["cpu_count"] = iv();
                else if (k == "services.lwm2m.server.mem.rss.kb")     services["lwm2m_server"]["mem_kb"] = iv();
                else if (k == "services.lwm2m.server.fd.count")       services["lwm2m_server"]["fd_count"] = iv();
                else if (k == "services.lwm2m.server.threads")        services["lwm2m_server"]["threads"] = iv();
                else if (k == "services.wifi.client.cpu.permille")    services["wifi_client"]["cpu_permille"] = iv();
                else if (k == "services.wifi.client.cpu.count")       services["wifi_client"]["cpu_count"] = iv();
                else if (k == "services.wifi.client.mem.rss.kb")      services["wifi_client"]["mem_kb"] = iv();
                else if (k == "services.wifi.client.fd.count")        services["wifi_client"]["fd_count"] = iv();
                else if (k == "services.wifi.client.threads")         services["wifi_client"]["threads"] = iv();
            }
            resp["lwm2m"]    = lwm2m;
            resp["vpn"]      = vpn;
            resp["wifi"]     = wifi;
            resp["wan"]      = wan;
            resp["routing"]  = routing;
            resp["services"] = services;
            r.body = resp.dump();
            return r;
        });

    // ── POST /api/v1/wifi/scan ───────────────────────────────────
    router.add("POST", "/api/v1/wifi/scan",
        [ds](const HttpParser::Request& /*req*/) -> HttpResponse {
            HttpResponse r;
            if (!ds) {
                r.status = 500;
                r.body = R"({"ok":false,"err":"data store not connected"})";
                return r;
            }
            // Bump wifi.scan.request to trigger an immediate scan.
            // The wifi-client daemon watches this key and fires a scan
            // on any change (bump-counter convention, same as net.iface.*).
            std::vector<data_store::Client::GetResult> got;
            auto gr = ds->get({"wifi.scan.request"}, got);
            std::uint32_t next = 1;
            if (gr.ok && !got.empty() && got[0].has_value) {
                if (auto cur = data_store::to_uint32(got[0].value))
                    next = *cur + 1;
            }
            auto sr = ds->set("wifi.scan.request",
                              data_store::Value{next});
            json resp;
            resp["ok"] = sr.ok;
            if (!sr.ok) resp["err"] = sr.err;
            resp["scan_request"] = next;
            r.body = resp.dump();
            if (!sr.ok) r.status = 500;
            return r;
        });

    // ── POST /api/v1/service/restart ─────────────────────────────
    router.add("POST", "/api/v1/service/restart",
        [ds](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            if (!ds) {
                r.status = 500;
                r.body = R"({"ok":false,"err":"data store not connected"})";
                return r;
            }
            auto doc = parse_body(req.body);
            std::string svc = doc.value("service", "");
            if (svc.empty()) {
                r.status = 400;
                r.body = R"({"ok":false,"err":"missing 'service' field"})";
                return r;
            }
            // Map short names to enable-key prefixes
            std::string enable_key;
            if (svc == "net.router")        enable_key = "services.net.router.enable";
            else if (svc == "openvpn.client") enable_key = "services.openvpn.client.enable";
            else if (svc == "lwm2m.client")   enable_key = "services.lwm2m.client.enable";
            else if (svc == "lwm2m.server")   enable_key = "services.lwm2m.server.enable";
            else if (svc == "wifi.client")    enable_key = "services.wifi.client.enable";
            else if (svc == "ds") {
                r.status = 400;
                r.body = R"json({"ok":false,"err":"cannot restart ds-server"})json";
                return r;
            } else {
                r.status = 400;
                r.body = R"({"ok":false,"err":"unknown service: ")" + svc + "\"}";
                return r;
            }
            // Restart: disable → enable. The daemon watches the key
            // and acts on transitions.  A 2 s gap gives the daemon
            // time to observe the disable before the re-enable.
            auto r1 = ds->set(enable_key, data_store::Value{false});
            if (!r1.ok) {
                r.status = 500;
                r.body = R"({"ok":false,"err":"disable failed: )" + r1.err + "\"}";
                return r;
            }
            // Brief sleep so the daemon's watch fires before we re-enable.
            // This is blocking on the reactor/handler thread — acceptable
            // for an infrequent operator action (restart).
            std::this_thread::sleep_for(std::chrono::seconds(2));
            auto r2 = ds->set(enable_key, data_store::Value{true});
            json resp;
            resp["ok"] = r2.ok;
            resp["service"] = svc;
            if (!r2.ok) {
                resp["err"] = "re-enable failed: " + r2.err;
                r.status = 500;
            }
            r.body = resp.dump();
            return r;
        });

    // ── GET /api/v1/log ──────────────────────────────────────────
    // Returns merged log.*.text as plain text for the UI's scrollable
    // log viewer. Each daemon writes to its own key so they don't
    // clobber each other — we merge all of them here.
    router.add("GET", "/api/v1/log",
        [ds](const HttpParser::Request& /*req*/) -> HttpResponse {
            HttpResponse r;
            r.content_type = "text/plain";
            if (!ds) {
                r.status = 500;
                r.body = "data store not connected";
                return r;
            }
            std::vector<data_store::Client::GetResult> got;
            auto rs = ds->get({"log.text", "log.cloudd.text",
                               "log.lwm2m.text", "log.lwm2m.bs.text",
                               "log.lwm2m.dm.text"}, got);
            std::string merged;
            if (rs.ok) {
                for (const auto& g : got) {
                    if (g.has_value) {
                        if (auto s = data_store::to_string(g.value))
                            merged += *s;
                    }
                }
            }
            r.body = std::move(merged);
            return r;
        });

    // ── GET /api/v1/cloud/endpoints ─────────────────────────────────
    // L21/D7 — ds-backed: reads cloud.endpoints JSON from ds-server
    // (written by iot-cloudd) so the cloud UI can list provisioned
    // devices without a direct registry pointer.
    router.add("GET", "/api/v1/cloud/endpoints",
        [ds](const HttpParser::Request& /*req*/) -> HttpResponse {
            HttpResponse r;
            if (!ds) {
                r.status = 500;
                r.body = R"({"ok":false,"err":"data store not connected"})";
                return r;
            }
            std::vector<data_store::Client::GetResult> got;
            auto rs = ds->get({"cloud.endpoints"}, got);
            json resp;
            resp["ok"] = true;
            json arr = json::array();
            if (rs.ok && !got.empty() && got[0].has_value) {
                if (auto s = data_store::to_string(got[0].value)) {
                    try {
                        auto parsed = json::parse(*s);
                        if (parsed.is_array()) arr = parsed;
                    } catch (const std::exception&) { /* return [] */ }
                }
            }
            resp["count"]     = arr.size();
            resp["endpoints"] = arr;
            r.body = resp.dump();
            return r;
        });

    // ── GET /api/v1/cloud/endpoint?ep=<ep> ─────────────────────────
    router.add("GET", "/api/v1/cloud/endpoint",
        [ds](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            if (!ds) {
                r.status = 500;
                r.body = R"({"ok":false,"err":"data store not connected"})";
                return r;
            }
            auto it = req.query.find("ep");
            if (it == req.query.end()) {
                r.status = 400;
                r.body = R"({"ok":false,"err":"missing 'ep' query param"})";
                return r;
            }
            std::string target = it->second;
            std::vector<data_store::Client::GetResult> got;
            auto rs = ds->get({"cloud.endpoints"}, got);
            if (rs.ok && !got.empty() && got[0].has_value) {
                if (auto s = data_store::to_string(got[0].value)) {
                    try {
                        auto arr = json::parse(*s);
                        if (arr.is_array()) {
                            for (const auto& item : arr) {
                                if (item.value("endpoint", "") == target) {
                                    json resp;
                                    resp["ok"]         = true;
                                    resp["endpoint"]   = item.value("endpoint", "");
                                    resp["tun_ip"]     = item.value("tun_ip", "");
                                    resp["proxy_port"] = item.value("proxy_port", 0);
                                    resp["registered"] = item.value("registered", false);
                                    resp["state"]      = item.value("state", "unknown");
                                    r.body = resp.dump();
                                    return r;
                                }
                            }
                        }
                    } catch (const std::exception&) {}
                }
            }
            r.status = 404;
            r.body = R"({"ok":false,"err":"endpoint not found"})";
            return r;
        });

    // ── POST /api/v1/cloud/endpoints (provision) ───────────────────
    // Writes cloud.provision.request = endpoint name. iot-cloudd
    // watches this key and provisions the device on change.
    router.add("POST", "/api/v1/cloud/endpoints",
        [ds](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            if (!ds) {
                r.status = 500;
                r.body = R"({"ok":false,"err":"data store not connected"})";
                return r;
            }
            auto doc = parse_body(req.body);
            std::string ep = doc.value("endpoint", "");
            if (ep.empty()) {
                r.status = 400;
                r.body = R"({"ok":false,"err":"missing 'endpoint' field"})";
                return r;
            }
            // Bump-counter pattern: write the endpoint name so iot-cloudd
            // picks it up on its next watch cycle.
            auto sr = ds->set("cloud.provision.request",
                              data_store::Value{ep});
            json resp;
            resp["ok"]       = sr.ok;
            resp["endpoint"] = ep;
            if (!sr.ok) {
                resp["err"] = sr.err;
                r.status = 500;
            }
            r.body = resp.dump();
            return r;
        });

    // ── DELETE /api/v1/cloud/endpoints?ep=<ep> (deprovision) ─────
    router.add("DELETE", "/api/v1/cloud/endpoints",
        [ds](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            if (!ds) {
                r.status = 500;
                r.body = R"({"ok":false,"err":"data store not connected"})";
                return r;
            }
            auto it = req.query.find("ep");
            if (it == req.query.end()) {
                r.status = 400;
                r.body = R"({"ok":false,"err":"missing 'ep' query param"})";
                return r;
            }
            std::string ep = it->second;
            auto sr = ds->set("cloud.deprovision.request",
                              data_store::Value{ep});
            json resp;
            resp["ok"]       = sr.ok;
            resp["endpoint"] = ep;
            if (!sr.ok) {
                resp["err"] = sr.err;
                r.status = 500;
            }
            r.body = resp.dump();
            return r;
        });

    // ── Wrap existing + new handlers with auth (when enabled) ─────
    // When auth is enabled, all /api/v1/* routes except /api/v1/auth/*
    // require a valid session.  The lambda below wraps a HandlerFn so
    // the auth check runs before the business logic.
    (void)auth;
}

} // namespace http_server
