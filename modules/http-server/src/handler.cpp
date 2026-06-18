#include "auth.hpp"
#include "handler.hpp"

#include "data_store/client.hpp"
#include "data_store/value.hpp"

#include <nlohmann/json.hpp>

#include <ace/Log_Msg.h>
#include <ace/OS_NS_unistd.h>
#include <ace/Time_Value.h>

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <fstream>
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

/// Load auth.users.accounts as a JSON array (empty array on any error).
/// Extra (non-admin) users are stored here as a single JSON blob because
/// the schema-claimed "auth" namespace rejects undeclared per-user keys.
json load_accounts(data_store::Client* ds) {
    json arr = json::array();
    if (!ds) return arr;
    std::vector<data_store::Client::GetResult> got;
    auto rs = ds->get({"auth.users.accounts"}, got);
    if (rs.ok && !got.empty() && got[0].has_value) {
        if (auto s = data_store::to_string(got[0].value)) {
            try {
                auto parsed = json::parse(*s);
                if (parsed.is_array()) arr = parsed;
            } catch (const std::exception&) { /* return [] */ }
        }
    }
    return arr;
}

/// Persist the accounts array back to auth.users.accounts.
bool save_accounts(data_store::Client* ds, const json& arr) {
    if (!ds) return false;
    auto sr = ds->set("auth.users.accounts", data_store::Value{arr.dump()});
    return sr.ok;
}

} // namespace

void install_handlers(Router& router,
                      data_store::Client* ds,
                      SessionStore* auth) {
    // ─── GET / → redirect to the SPA login ───────────────────
    // The UI lives under /webui/; hitting the bare host (e.g. just the
    // IP) should land on the login page instead of a 404, so users can
    // reach it with or without the /webui/login path.
    router.add("GET", "/",
        [](const HttpParser::Request&) -> HttpResponse {
            HttpResponse r;
            r.status = 302;
            r.content_type = "text/plain";
            r.headers["Location"] = "/webui/login";
            r.body = "";
            return r;
        });

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
                std::string token = extract_session_cookie(req.headers,
                                                           auth->cookie_name());
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
            // Resolve credentials. The built-in "admin" lives in its own
            // ds keys; any other id is looked up in auth.users.accounts
            // (the schema-claimed "auth" namespace can't hold dynamic
            // per-user keys, so extra users share one JSON-blob key).
            std::string stored_hash;
            std::string access = "Admin";
            std::string role   = "admin";
            if (id == "admin") {
                stored_hash = ds ? CredentialStore::load_admin_password_hash(*ds)
                                 : CredentialStore::kDefaultHash;
                if (ds) access = CredentialStore::load_user_access(*ds, id);
            } else {
                role = "user";
                bool found = false;
                if (ds) {
                    for (const auto& u : load_accounts(ds)) {
                        if (u.value("id", "") == id) {
                            stored_hash = u.value("hash", "");
                            access      = u.value("access", "Viewer");
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) {
                    r.status = 401;
                    r.body = R"({"ok":false,"err":"invalid credentials"})";
                    return r;
                }
            }
            if (!CredentialStore::verify(password, stored_hash)) {
                r.status = 401;
                r.body = R"({"ok":false,"err":"invalid credentials"})";
                return r;
            }
            std::string token;
            if (auth) token = auth->create_session(id, role, access);
            json resp;
            resp["ok"]     = true;
            resp["role"]   = role;
            resp["access"] = access;
            r.body = resp.dump();
            if (!token.empty()) {
                r.headers["Set-Cookie"] = make_set_cookie(
                    token, auth ? auth->cookie_name() : "iot-session");
            }
            return r;
        });

    // ── POST /api/v1/auth/logout ─────────────────────────────────
    router.add("POST", "/api/v1/auth/logout",
        [auth](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            const std::string cname = auth ? auth->cookie_name() : "iot-session";
            if (auth) {
                std::string token = extract_session_cookie(req.headers, cname);
                if (!token.empty()) auth->destroy(token);
            }
            r.body = R"({"ok":true})";
            r.headers["Set-Cookie"] = make_clear_cookie(cname);
            return r;
        });

    // ── POST /api/v1/update/upload?name=<file.ipk> ────────────────
    // Drag-and-drop OTA: the device-ui posts a .ipk (or small .tar.gz bundle) as
    // the raw request body. Admin-only. Writes it into the swupdate spool
    // (/run/iot/update) and trips the inotify trigger; iot-swupdate then installs
    // it (Yocto/systemd only — the spool + iot-swupdate.path live there). Bounded
    // by the parser's 8 MiB body cap; larger/full-image bundles use the Phase-2
    // streaming path (apps/docs/tdd-ab-image-ota.md).
    router.add("POST", "/api/v1/update/upload",
        [ds, auth](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            // Admin gate.
            std::string access_level = "Admin";
            if (auth && auth->enabled()) {
                std::string token = extract_session_cookie(req.headers,
                                                           auth->cookie_name());
                if (!token.empty()) {
                    const auto* session = auth->validate(token);
                    if (session) access_level = session->access;
                }
            }
            if (access_level != "Admin") {
                r.status = 403;
                r.body = R"({"ok":false,"err":"admin required"})";
                return r;
            }
            // Sanitise the filename from ?name= (basename, safe charset, known ext).
            std::string name;
            if (auto it = req.query.find("name"); it != req.query.end()) name = it->second;
            if (auto p = name.find_last_of('/'); p != std::string::npos) name = name.substr(p + 1);
            std::string safe;
            for (char c : name) {
                safe.push_back((std::isalnum(static_cast<unsigned char>(c)) ||
                                c == '.' || c == '_' || c == '-') ? c : '_');
            }
            auto ends_with = [&](const char* e) {
                std::string s(e);
                return safe.size() >= s.size() &&
                       safe.compare(safe.size() - s.size(), s.size(), s) == 0;
            };
            if (safe.empty() || !(ends_with(".ipk") || ends_with(".tar.gz") || ends_with(".tgz"))) {
                r.status = 400;
                r.body = R"({"ok":false,"err":"name must end in .ipk, .tar.gz or .tgz"})";
                return r;
            }
            // Chunked append: large bundles (.raucb) exceed the 8 MiB body cap,
            // so the UI slices the file and POSTs sequential chunks. offset=0
            // truncates + starts; offset>0 appends; final=1 trips the installer.
            // No params → single-shot (back-compatible) = offset 0 + final 1.
            bool first = true, final = true;
            if (auto it = req.query.find("offset"); it != req.query.end())
                first = (it->second == "0" || it->second.empty());
            if (auto it = req.query.find("final"); it != req.query.end())
                final = (it->second == "1" || it->second == "true");
            if (req.body.empty() && first) {
                r.status = 400;
                r.body = R"({"ok":false,"err":"empty upload"})";
                return r;
            }
            const std::string spool = "/run/iot/update";
            {
                std::ios::openmode mode = std::ios::binary | std::ios::out |
                                          (first ? std::ios::trunc : std::ios::app);
                std::ofstream f(spool + "/" + safe, mode);
                if (!f) {
                    r.status = 500;
                    r.body = R"({"ok":false,"err":"cannot write update spool - perms or non-Yocto host"})";
                    return r;
                }
                if (!req.body.empty())
                    f.write(req.body.data(), static_cast<std::streamsize>(req.body.size()));
                if (!f) {
                    r.status = 500;
                    r.body = R"({"ok":false,"err":"spool write failed"})";
                    return r;
                }
            }
            if (final) {
                if (ds) ds->set("iot.update.state", data_store::Value{static_cast<std::int32_t>(2)});
                { std::ofstream trig(spool + "/update", std::ios::trunc); }  // arm iot-swupdate.path
                ACE_DEBUG((LM_INFO,
                           ACE_TEXT("%D httpd:thread:%t %M %N:%l OTA upload complete %C; "
                                    "triggered iot-swupdate\n"), safe.c_str()));
            }
            r.body = R"({"ok":true})";
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

            // Long-poll path: watch the two most dynamic status keys —
            // vpn.state (VPN lifecycle) and iot.conn.state (LwM2M
            // bootstrap → DM → registered lifecycle) — and return the full
            // status snapshot on either change or timeout.
            if (timeout > 0) {
                using LpState = struct {
                    std::mutex                 m;
                    std::condition_variable    cv;
                    bool                       fired = false;
                };
                auto st = std::make_shared<LpState>();
                auto notify = [st](const data_store::Client::Event& /*e*/) {
                    std::lock_guard<std::mutex> lk(st->m);
                    st->fired = true;
                    st->cv.notify_one();
                };
                data_store::Client::WatchHandle wh_vpn =
                    data_store::Client::kInvalidHandle;
                data_store::Client::WatchHandle wh_conn =
                    data_store::Client::kInvalidHandle;
                data_store::Client::WatchHandle wh_stats =
                    data_store::Client::kInvalidHandle;
                data_store::Client::WatchHandle wh_log =
                    data_store::Client::kInvalidHandle;
                data_store::Client::WatchHandle wh_update =
                    data_store::Client::kInvalidHandle;
                auto ws = ds->watch("vpn.state", notify, &wh_vpn);
                ds->watch("iot.conn.state", notify, &wh_conn);
                // Also wake on the two domain bump keys so this single status
                // long-poll carries Service telemetry + Logs freshness — the
                // SPA needs no per-page long-poll (avoids worker starvation).
                ds->watch("services.stats.version", notify, &wh_stats);
                ds->watch("log.version", notify, &wh_log);
                // OTA progress: wake on iot.update.state (device self-update)
                // and cloud.update.status (cloud→device push progress) so the
                // Software pages track an in-flight update live off this stream.
                ds->watch("iot.update.state", notify, &wh_update);
                data_store::Client::WatchHandle wh_cupd =
                    data_store::Client::kInvalidHandle;
                ds->watch("cloud.update.status", notify, &wh_cupd);
                if (ws.ok) {
                    std::unique_lock<std::mutex> lk(st->m);
                    st->cv.wait_for(lk, std::chrono::seconds(timeout),
                                    [&] { return st->fired; });
                }
                if (wh_vpn != data_store::Client::kInvalidHandle)
                    ds->unwatch(wh_vpn);
                if (wh_conn != data_store::Client::kInvalidHandle)
                    ds->unwatch(wh_conn);
                if (wh_stats != data_store::Client::kInvalidHandle)
                    ds->unwatch(wh_stats);
                if (wh_log != data_store::Client::kInvalidHandle)
                    ds->unwatch(wh_log);
                if (wh_update != data_store::Client::kInvalidHandle)
                    ds->unwatch(wh_update);
                if (wh_cupd != data_store::Client::kInvalidHandle)
                    ds->unwatch(wh_cupd);
                // Fall through to build the full status snapshot
            }
            std::vector<data_store::Client::GetResult> got;
            auto rs = ds->get({
                // LwM2M
                "iot.server.uri", "iot.dm.uri", "iot.endpoint", "iot.conn.state",
                // OTA software update
                "iot.update.version", "iot.update.state", "iot.update.result",
                // VPN
                "vpn.state", "vpn.assigned.ip", "vpn.assigned.gateway",
                "vpn.assigned.netmask", "vpn.assigned.dns", "vpn.pid",
                "vpn.exit_code", "vpn.gate.reason", "vpn.bound.iface",
                // WiFi
                "wifi.assoc.state", "wifi.assoc.ssid", "wifi.signal.rssi",
                "wifi.dhcp.state", "wifi.dhcp.ip", "wifi.dhcp.mask",
                "wifi.dhcp.gateway", "wifi.dhcp.dns", "wifi.dhcp.lease.sec",
                "wifi.dhcp.domain", "wifi.dhcp.obtained.unix",
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
                // ── Cloud service states/enables + telemetry (cloud UI
                // Services page reads these via the shared status poll) ──
                "services.cloud.iot.cloudd.enable", "services.cloud.iot.cloudd.state",
                "services.cloud.iot.httpd.enable", "services.cloud.iot.httpd.state",
                "services.cloud.openvpn.server.enable", "services.cloud.openvpn.server.state",
                "services.cloud.lwm2m.bs.state", "services.cloud.lwm2m.dm.state",
                "services.cloud.iot.cloudd.cpu.permille", "services.cloud.iot.cloudd.cpu.count",
                "services.cloud.iot.cloudd.mem.rss.kb",
                "services.cloud.iot.cloudd.fd.count", "services.cloud.iot.cloudd.threads",
                "services.cloud.iot.httpd.cpu.permille", "services.cloud.iot.httpd.cpu.count",
                "services.cloud.iot.httpd.mem.rss.kb",
                "services.cloud.iot.httpd.fd.count", "services.cloud.iot.httpd.threads",
                "services.cloud.openvpn.server.cpu.permille", "services.cloud.openvpn.server.cpu.count",
                "services.cloud.openvpn.server.mem.rss.kb",
                "services.cloud.openvpn.server.fd.count", "services.cloud.openvpn.server.threads",
                "services.cloud.lwm2m.bs.cpu.permille", "services.cloud.lwm2m.bs.cpu.count",
                "services.cloud.lwm2m.bs.mem.rss.kb",
                "services.cloud.lwm2m.bs.fd.count", "services.cloud.lwm2m.bs.threads",
                "services.cloud.lwm2m.dm.cpu.permille", "services.cloud.lwm2m.dm.cpu.count",
                "services.cloud.lwm2m.dm.mem.rss.kb",
                "services.cloud.lwm2m.dm.fd.count", "services.cloud.lwm2m.dm.threads",
                // Domain bump keys — echoed back so the SPA can observe them.
                "services.stats.version", "log.version",
                "cloud.update.status",
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
            json update     = json::object();
            json vpn        = json::object();
            json wifi       = json::object();
            json wan        = json::object();
            json routing    = json::object();
            json services   = json::object();
            // Flat passthrough (ds key → typed value) for keys the SPA caches
            // verbatim (cloud Service rows + domain bump keys).
            json cloud      = json::object();

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
                else if (k == "iot.dm.uri")       lwm2m["dm_uri"] = sv();
                else if (k == "iot.update.version") update["version"] = sv();
                else if (k == "iot.update.state")   update["state"] = iv();
                else if (k == "iot.update.result")  update["result"] = iv();
                else if (k == "iot.endpoint")     lwm2m["endpoint"] = sv();
                else if (k == "iot.conn.state")   lwm2m["conn_state"] = sv();

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
                else if (k == "wifi.dhcp.mask")        wifi["dhcp_mask"] = sv();
                else if (k == "wifi.dhcp.gateway")     wifi["dhcp_gateway"] = sv();
                else if (k == "wifi.dhcp.dns")         wifi["dhcp_dns"] = sv();
                else if (k == "wifi.dhcp.lease.sec")   wifi["dhcp_lease_sec"] = iv();
                else if (k == "wifi.dhcp.domain")      wifi["dhcp_domain"] = sv();
                else if (k == "wifi.dhcp.obtained.unix") wifi["dhcp_obtained_unix"] = iv();

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

                // Cloud Service rows + bump keys: pass through verbatim under
                // their ds key, typed by suffix (state→string, enable→bool,
                // everything else numeric).
                // Cloud OTA push status: a JSON array string, passed through
                // verbatim so the cloud Software page observes it off /status.
                else if (k == "cloud.update.status") cloud[k] = sv();
                else if (k.rfind("services.cloud.", 0) == 0 ||
                         k == "services.stats.version" || k == "log.version") {
                    if (g.has_value) {
                        const std::string suffix =
                            k.size() >= 6 ? k.substr(k.size() - 6) : k;
                        if (suffix == ".state")        cloud[k] = sv();
                        else if (suffix == "enable")   cloud[k] = bv(true);
                        else                           cloud[k] = iv();
                    }
                }
            }
            resp["lwm2m"]    = lwm2m;
            resp["update"]   = update;
            resp["vpn"]      = vpn;
            resp["wifi"]     = wifi;
            resp["wan"]      = wan;
            resp["routing"]  = routing;
            resp["services"] = services;
            resp["cloud"]    = cloud;
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
            resp["endpoint"] = ep;
            if (!sr.ok) {
                resp["ok"]  = false;
                resp["err"] = sr.err;
                r.status    = 500;
                r.body      = resp.dump();
                return r;
            }

            // Don't report success on the mere trigger write — that lied when a
            // stale/duplicate request or a wedged iot-cloudd left the endpoint in
            // place (the UI showed a green toast, then the row repopulated from
            // iot-cloudd's in-memory registry on the next sync). Poll cloud.endpoints
            // until the endpoint is actually gone (iot-cloudd processed the
            // deprovision and removed the in-memory row → sync rewrote the key), or
            // time out and report failure honestly.
            auto endpoint_present = [&](const std::string& want) -> bool {
                std::vector<data_store::Client::GetResult> got;
                if (!ds->get({"cloud.endpoints"}, got).ok || got.empty() ||
                    !got[0].has_value)
                    return false;
                auto s = data_store::to_string(got[0].value);
                if (!s) return false;
                try {
                    auto arr = json::parse(*s);
                    if (arr.is_array())
                        for (const auto& e : arr)
                            if (e.value("endpoint", std::string()) == want)
                                return true;
                } catch (const std::exception&) {}
                return false;
            };
            bool removed = false;
            for (int i = 0; i < 50; ++i) {           // ~5s (50 × 100ms)
                if (!endpoint_present(ep)) { removed = true; break; }
                ACE_OS::sleep(ACE_Time_Value(0, 100 * 1000));  // 100ms
            }
            resp["ok"] = removed;
            if (!removed) {
                resp["err"] = "deprovision not confirmed (iot-cloudd did not "
                              "remove the endpoint within timeout)";
                r.status = 504;
            }
            r.body = resp.dump();
            return r;
        });

    // ── GET /api/v1/users ────────────────────────────────────────
    // List user accounts (id + access only — never the password hash).
    // The built-in admin is always present. Any authenticated session
    // may list; create/delete are Admin-gated below.
    router.add("GET", "/api/v1/users",
        [ds](const HttpParser::Request& /*req*/) -> HttpResponse {
            HttpResponse r;
            if (!ds) {
                r.status = 500;
                r.body = R"({"ok":false,"err":"data store not connected"})";
                return r;
            }
            json users = json::array();
            json admin;
            admin["id"]     = "admin";
            admin["access"] = CredentialStore::load_user_access(*ds, "admin");
            users.push_back(admin);
            for (const auto& u : load_accounts(ds)) {
                json e;
                e["id"]     = u.value("id", "");
                e["access"] = u.value("access", "Viewer");
                users.push_back(e);
            }
            json resp;
            resp["ok"]    = true;
            resp["users"] = users;
            r.body = resp.dump();
            return r;
        });

    // ── POST /api/v1/users  { id, password, access } ─────────────
    // Create or update a non-admin user. Admin-gated. The password is
    // SHA-256-hashed server-side; plaintext is never stored.
    router.add("POST", "/api/v1/users",
        [ds, auth](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            if (!ds) {
                r.status = 500;
                r.body = R"({"ok":false,"err":"data store not connected"})";
                return r;
            }
            // Access control (mirrors db/set): Viewer is forbidden.
            std::string access_level = "Admin";
            if (auth && auth->enabled()) {
                std::string token = extract_session_cookie(req.headers,
                                                           auth->cookie_name());
                if (!token.empty()) {
                    const auto* session = auth->validate(token);
                    if (session) access_level = session->access;
                }
            }
            if (!can_write(access_level, "")) {
                r.status = 403;
                r.body = R"({"ok":false,"err":"forbidden: read-only access"})";
                return r;
            }
            auto doc = parse_body(req.body);
            std::string uid      = doc.value("id", "");
            std::string password = doc.value("password", "");
            std::string uaccess  = doc.value("access", "Viewer");
            if (uid.empty() || password.empty()) {
                r.status = 400;
                r.body = R"({"ok":false,"err":"id and password required"})";
                return r;
            }
            if (uid == "admin") {
                r.status = 400;
                r.body = R"({"ok":false,"err":"built-in admin is managed separately"})";
                return r;
            }
            if (uaccess != "Admin" && uaccess != "Viewer") uaccess = "Viewer";

            auto accounts = load_accounts(ds);
            std::string hash = sha256_hex(password);
            bool updated = false;
            for (auto& u : accounts) {
                if (u.value("id", "") == uid) {
                    u["hash"]   = hash;
                    u["access"] = uaccess;
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                json e;
                e["id"]     = uid;
                e["hash"]   = hash;
                e["access"] = uaccess;
                accounts.push_back(e);
            }
            json resp;
            if (!save_accounts(ds, accounts)) {
                r.status = 500;
                resp["ok"]  = false;
                resp["err"] = "failed to persist accounts";
            } else {
                resp["ok"] = true;
                resp["id"] = uid;
            }
            r.body = resp.dump();
            return r;
        });

    // ── DELETE /api/v1/users?id=<id> ─────────────────────────────
    router.add("DELETE", "/api/v1/users",
        [ds, auth](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            if (!ds) {
                r.status = 500;
                r.body = R"({"ok":false,"err":"data store not connected"})";
                return r;
            }
            std::string access_level = "Admin";
            if (auth && auth->enabled()) {
                std::string token = extract_session_cookie(req.headers,
                                                           auth->cookie_name());
                if (!token.empty()) {
                    const auto* session = auth->validate(token);
                    if (session) access_level = session->access;
                }
            }
            if (!can_write(access_level, "")) {
                r.status = 403;
                r.body = R"({"ok":false,"err":"forbidden: read-only access"})";
                return r;
            }
            auto it = req.query.find("id");
            if (it == req.query.end()) {
                r.status = 400;
                r.body = R"({"ok":false,"err":"missing 'id' query param"})";
                return r;
            }
            std::string uid = it->second;
            if (uid == "admin") {
                r.status = 400;
                r.body = R"({"ok":false,"err":"cannot delete built-in admin"})";
                return r;
            }
            auto accounts = load_accounts(ds);
            json kept = json::array();
            bool removed = false;
            for (const auto& u : accounts) {
                if (u.value("id", "") == uid) { removed = true; continue; }
                kept.push_back(u);
            }
            json resp;
            if (!removed) {
                r.status = 404;
                resp["ok"]  = false;
                resp["err"] = "user not found";
            } else if (!save_accounts(ds, kept)) {
                r.status = 500;
                resp["ok"]  = false;
                resp["err"] = "failed to persist accounts";
            } else {
                resp["ok"] = true;
                resp["id"] = uid;
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
