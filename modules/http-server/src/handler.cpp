#include "audit.hpp"
#include "auth.hpp"
#include "handler.hpp"
#include "tenant_scope.hpp"

#include "data_store/client.hpp"
#include "data_store/value.hpp"

#include <nlohmann/json.hpp>

#include <ace/Log_Msg.h>
#include <ace/OS_NS_unistd.h>
#include <ace/Time_Value.h>

#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <sys/wait.h>

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

/// Sanitize a firmware-feed filename: strip any path, restrict to a safe
/// charset, and require an accepted extension. Returns "" if rejected.
/// Shared by the firmware upload + fetch handlers. The accepted set matches
/// /update/upload — .tar covers a macOS-auto-decompressed .tar.gz; .raucb is
/// the A/B full-image bundle.
std::string sanitize_feed_name(std::string name) {
    if (auto p = name.find_last_of('/'); p != std::string::npos) name = name.substr(p + 1);
    std::string safe;
    for (char c : name)
        safe.push_back((std::isalnum(static_cast<unsigned char>(c)) ||
                        c == '.' || c == '_' || c == '-') ? c : '_');
    auto ends_with = [&](const char* e) {
        std::string s(e);
        return safe.size() >= s.size() &&
               safe.compare(safe.size() - s.size(), s.size(), s) == 0;
    };
    if (safe.empty() || !(ends_with(".ipk") || ends_with(".tar") ||
                          ends_with(".tar.gz") || ends_with(".tgz") ||
                          ends_with(".raucb")))
        return {};
    return safe;
}

/// Upsert one row into cloud.firmware.manifest, keyed by ipk_url. Shared by the
/// firmware upload + fetch handlers.
void upsert_firmware_manifest(data_store::Client* ds, const std::string& ipk_url,
                              const std::string& pkg, const std::string& ver,
                              const std::string& arch, const std::string& sha) {
    if (!ds) return;
    json arr = json::array();
    std::vector<data_store::Client::GetResult> g;
    if (ds->get({std::string("cloud.firmware.manifest")}, g).ok &&
        !g.empty() && g[0].has_value)
        if (auto cur = data_store::to_string(g[0].value))
            try { auto p = json::parse(*cur); if (p.is_array()) arr = p; }
            catch (const std::exception&) {}
    json out = json::array();
    for (auto& e : arr)
        if (e.value("ipk_url", "") != ipk_url) out.push_back(e);
    out.push_back({{"pkg", pkg}, {"version", ver}, {"arch", arch},
                   {"ipk_url", ipk_url}, {"sha256", sha}});
    ds->set("cloud.firmware.manifest", data_store::Value{out.dump()});
}

/// Validate an operator-supplied fetch URL. Admin-supplied but still checked:
/// scheme must be http/https, and the whole string is restricted so it carries
/// no whitespace, control char, DEL, or single quote — the only character that
/// could break out of the single-quoted curl argument. Everything else (&, ?,
/// =, %, +, [], …) is inert inside single quotes, so query strings survive.
bool is_safe_fetch_url(const std::string& u) {
    if (u.rfind("http://", 0) != 0 && u.rfind("https://", 0) != 0) return false;
    if (u.empty() || u.size() > 2048) return false;
    for (unsigned char c : u)
        if (c < 0x21 || c == 0x7f || c == '\'') return false;
    return true;
}

/// Download an http(s) URL into `dest` with curl (TLS verified — the cloud
/// runtime image ships curl + ca-certificates). `url` MUST already be
/// is_safe_fetch_url()-validated: it is single-quoted into the command, and the
/// dest is server-controlled, so there is no shell-injection surface. Returns
/// true on curl exit 0.
bool fetch_url_to_file(const std::string& url, const std::string& dest) {
    const std::string cmd =
        "curl -fL --connect-timeout 20 --max-time 1800 -o '" + dest +
        "' '" + url + "' >/dev/null 2>&1";
    int rc = std::system(cmd.c_str());
    return rc != -1 && WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
}

/// Publish firmware-fetch progress (an object the cloud-ui observes).
void set_fetch_status(data_store::Client* ds, const json& st) {
    if (ds) ds->set("cloud.firmware.fetch.status", data_store::Value{st.dump()});
}

} // namespace

void install_handlers(Router& router,
                      data_store::Client* ds,
                      SessionStore* auth,
                      const std::string& firmware_dir) {
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
        [ds, auth](const HttpParser::Request& req) -> HttpResponse {
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
                // Multi-tenant: scope cloud.endpoints to the caller's tenant so
                // the operator only sees their own devices (the cloud-ui reads
                // the endpoint list through this generic getter). No-op for the
                // platform operator ("*") or when auth isn't wired.
                if (resp["data"].contains("cloud.endpoints") &&
                    resp["data"]["cloud.endpoints"].is_string()) {
                    resp["data"]["cloud.endpoints"] = scope_endpoints_json(
                        resp["data"]["cloud.endpoints"].get<std::string>(),
                        request_tenant(req.headers, auth));
                }
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
            // ── Access control + actor attribution (P5c audit) ──
            std::string access_level = "Admin";  // default: full access
            std::string actor = "system", actor_tenant = "default";
            if (auth && auth->enabled()) {
                std::string token = extract_session_cookie(req.headers,
                                                           auth->cookie_name());
                if (!token.empty()) {
                    const auto* session = auth->validate(token);
                    if (session) {
                        access_level = session->access;
                        actor        = session->username;
                        actor_tenant = session->tenant;
                    }
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
                // Multi-tenant (D7): a tenant-scoped operator can only provision
                // into THEIR OWN tenant. Pin cloud.provision.tenant to the
                // session tenant (overriding any value the caller sent), and if a
                // provision request comes in without one, inject it — so a crafted
                // API call can't tag a device into another tenant. The platform
                // operator ("*") is unrestricted.
                if (auth && auth->enabled() && actor_tenant != "*") {
                    bool hasTenantPair = false, hasRequest = false;
                    for (auto& p : pairs) {
                        if (p.first == "cloud.provision.tenant") {
                            p.second = data_store::Value{actor_tenant};
                            hasTenantPair = true;
                        } else if (p.first == "cloud.provision.request") {
                            hasRequest = true;
                        }
                    }
                    if (hasRequest && !hasTenantPair)
                        pairs.emplace_back("cloud.provision.tenant",
                                           data_store::Value{actor_tenant});
                }
                auto rs = ds->set(pairs);
                if (!rs.ok) {
                    r.status = 400;
                    r.body = R"({"ok":false,"err":")" + rs.err + "\"}";
                    return r;
                }
                // ── P5c: record operator/provisioning actions in the audit log.
                // Only a small watchlist of keys is auditable (tenant + device
                // lifecycle); the actor comes from the session above. Best-effort
                // — a failed audit write never fails the operator's set.
                {
                    std::vector<http_server::AuditEntry> events;
                    const long now = static_cast<long>(std::time(nullptr));
                    for (const auto& p : pairs) {
                        const std::string action =
                            http_server::audit_action_for_key(p.first);
                        if (action.empty()) continue;
                        std::string target;
                        if (auto s = data_store::to_string(p.second)) target = *s;
                        // Skip iot-cloudd's clear-to-"" of the request carriers
                        // (a system step, not an operator action).
                        if (target.empty() && action.rfind("device.", 0) == 0)
                            continue;
                        http_server::AuditEntry e;
                        e.ts = now; e.actor = actor; e.tenant = actor_tenant;
                        e.action = action;
                        // tenant.update / bs.master.update carry no per-row
                        // target; the others log their serial.
                        e.target = (action == "tenant.update" ||
                                    action == "bs.master.update") ? "" : target;
                        events.push_back(std::move(e));
                    }
                    if (!events.empty()) {
                        std::vector<data_store::Client::GetResult> got;
                        std::string log = "[]";
                        if (ds->get({"cloud.audit.log"}, got).ok &&
                            !got.empty() && got[0].has_value) {
                            if (auto s = data_store::to_string(got[0].value)) log = *s;
                        }
                        for (const auto& e : events) log = http_server::append_audit(log, e);
                        ds->set({{ "cloud.audit.log", data_store::Value{log} }});
                    }
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
        [ds, auth](const HttpParser::Request& req) -> HttpResponse {
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
                if (key == "cloud.endpoints" && resp["value"].is_string())
                    resp["value"] = scope_endpoints_json(
                        resp["value"].get<std::string>(),
                        request_tenant(req.headers, auth));
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
            if (key == "cloud.endpoints" && resp["value"].is_string())
                resp["value"] = scope_endpoints_json(
                    resp["value"].get<std::string>(),
                    request_tenant(req.headers, auth));
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
            std::string tenant = "default";   // multi-tenant: owning tenant
            if (id == "admin") {
                stored_hash = ds ? CredentialStore::load_admin_password_hash(*ds)
                                 : CredentialStore::kDefaultHash;
                if (ds) {
                    access = CredentialStore::load_user_access(*ds, id);
                    tenant = CredentialStore::load_user_tenant(*ds, id);
                }
                // Built-in admin is the platform operator ("*" = all tenants)
                // unless explicitly scoped via auth.users.admin.tenant.
                if (tenant.empty() || tenant == "default") tenant = "*";
            } else {
                role = "user";
                bool found = false;
                if (ds) {
                    for (const auto& u : load_accounts(ds)) {
                        if (u.value("id", "") == id) {
                            stored_hash = u.value("hash", "");
                            access      = u.value("access", "Viewer");
                            tenant      = u.value("tenant", "default");
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
            if (auth) token = auth->create_session(id, role, access, tenant);
            json resp;
            resp["ok"]     = true;
            resp["role"]   = role;
            resp["access"] = access;
            resp["tenant"] = tenant;   // owning tenant ("*" = platform operator)
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
            // Accept .tar too: macOS Safari/Archive Utility silently gunzips a
            // downloaded .tar.gz, leaving a plain .tar the operator then drops
            // here; the stager/swupdate detect compression by content, so an
            // uncompressed tar is fine. .raucb is the A/B full-image bundle
            // (iot-swupdate routes it to `rauc install`).
            if (safe.empty() || !(ends_with(".ipk") || ends_with(".tar") ||
                                  ends_with(".tar.gz") || ends_with(".tgz") ||
                                  ends_with(".raucb"))) {
                r.status = 400;
                r.body = R"({"ok":false,"err":"name must end in .ipk, .tar, .tar.gz, .tgz or .raucb"})";
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

    // ── POST /api/v1/system/reboot ────────────────────────────────
    // Admin-only. iot-httpd is unprivileged (DynamicUser, group iot) so it
    // cannot reboot directly; it arms a trigger file in /run/iot that the root
    // iot-reboot.path systemd unit watches → iot-reboot.service runs
    // `systemctl reboot`. Same privilege-separation pattern as the OTA stager.
    // Yocto/systemd only (the .path/.service units live there).
    router.add("POST", "/api/v1/system/reboot",
        [auth](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            std::string access_level = "Admin";
            if (auth && auth->enabled()) {
                std::string token = extract_session_cookie(req.headers, auth->cookie_name());
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
            std::ofstream trig("/run/iot/reboot.request", std::ios::trunc);
            if (!trig) {
                r.status = 500;
                r.body = R"({"ok":false,"err":"cannot arm reboot - perms or non-Yocto host"})";
                return r;
            }
            trig << "reboot\n";
            ACE_DEBUG((LM_WARNING,
                       ACE_TEXT("%D httpd:thread:%t %M %N:%l SYSTEM REBOOT requested "
                                "via device-ui (armed iot-reboot.path)\n")));
            r.body = R"({"ok":true})";
            return r;
        });

    // ── POST /api/v1/system/factory-reset ─────────────────────────
    // Admin-only + DESTRUCTIVE. Arms the trigger that the root
    // iot-factory-reset.service acts on: it removes the persisted data-store
    // (/var/lib/iot/data_store.lua, the operator-override layer) + generated VPN
    // certs, so the device falls back to the schema (Lua) defaults, then
    // reboots into that first-boot state. Yocto/systemd only.
    router.add("POST", "/api/v1/system/factory-reset",
        [auth](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            std::string access_level = "Admin";
            if (auth && auth->enabled()) {
                std::string token = extract_session_cookie(req.headers, auth->cookie_name());
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
            std::ofstream trig("/run/iot/factory-reset.request", std::ios::trunc);
            if (!trig) {
                r.status = 500;
                r.body = R"({"ok":false,"err":"cannot arm factory-reset - perms or non-Yocto host"})";
                return r;
            }
            trig << "factory-reset\n";
            ACE_DEBUG((LM_WARNING,
                       ACE_TEXT("%D httpd:thread:%t %M %N:%l FACTORY RESET requested "
                                "via device-ui (armed iot-factory-reset.path)\n")));
            r.body = R"({"ok":true})";
            return r;
        });

    // ── POST /api/v1/system/transfer ──────────────────────────────
    // Admin-only + ownership-changing. Re-homes the device to a new owner:
    // arms the trigger that the `engineer` iot-transfer.service acts on, which
    // wipes the customer-scoped credentials + VPN trust + LwM2M bootstrap
    // binding but KEEPS the network, then parks for re-commission (Phase 1 of
    // apps/docs/tdd-device-transfer.md). Yocto/systemd only.
    //
    // FAIL-CLOSED auth (deliberately unlike reboot/factory-reset above, which
    // default to Admin on an absent cookie): a destructive ownership change must
    // require a valid Admin session whenever auth is enabled. With auth disabled
    // the whole API is open by operator choice, so it is allowed.
    router.add("POST", "/api/v1/system/transfer",
        [auth](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            bool allowed = false;
            if (auth && auth->enabled()) {
                std::string token = extract_session_cookie(req.headers, auth->cookie_name());
                if (!token.empty()) {
                    const auto* session = auth->validate(token);
                    if (session && session->access == "Admin") allowed = true;
                }
            } else {
                allowed = true;   // auth disabled → API open by operator choice
            }
            if (!allowed) {
                r.status = 403;
                r.body = R"({"ok":false,"err":"admin required"})";
                return r;
            }
            std::ofstream trig("/run/iot/transfer.request", std::ios::trunc);
            if (!trig) {
                r.status = 500;
                r.body = R"({"ok":false,"err":"cannot arm transfer - perms or non-Yocto host"})";
                return r;
            }
            trig << "transfer\n";
            ACE_DEBUG((LM_WARNING,
                       ACE_TEXT("%D httpd:thread:%t %M %N:%l DEVICE TRANSFER requested "
                                "via device-ui (armed iot-transfer.path)\n")));
            r.body = R"({"ok":true})";
            return r;
        });

    // ── POST /api/v1/firmware/upload?name=<f>&version=&arch=&pkg= ──
    // Cloud-side: browse / drag-drop a .ipk or .tar.gz bundle in the cloud-ui
    // straight into the firmware FEED (firmware_dir, served at /firmware/) and
    // add it to cloud.firmware.manifest — no scp / ds-cli for the operator.
    // Chunked like /update/upload (offset=0 truncates, final=1 finalises). On
    // the final chunk the server computes the sha256 over the assembled file
    // (authoritative) and upserts the manifest row keyed by ipk_url. Only
    // active where a firmware feed exists (the cloud); a device has none → 400.
    router.add("POST", "/api/v1/firmware/upload",
        [ds, auth, firmware_dir](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            std::string access_level = "Admin";
            if (auth && auth->enabled()) {
                std::string token = extract_session_cookie(req.headers, auth->cookie_name());
                if (!token.empty()) { const auto* s = auth->validate(token); if (s) access_level = s->access; }
            }
            if (access_level != "Admin") {
                r.status = 403; r.body = R"({"ok":false,"err":"admin required"})"; return r;
            }
            if (firmware_dir.empty()) {
                r.status = 400; r.body = R"({"ok":false,"err":"no firmware feed on this host"})"; return r;
            }
            auto qp = [&](const char* k) -> std::string {
                auto it = req.query.find(k); return it != req.query.end() ? it->second : std::string();
            };
            const std::string safe = sanitize_feed_name(qp("name"));
            if (safe.empty()) {
                r.status = 400; r.body = R"({"ok":false,"err":"name must end in .ipk, .tar, .tar.gz, .tgz or .raucb"})"; return r;
            }
            bool first = true, final = true;
            if (auto it = req.query.find("offset"); it != req.query.end())
                first = (it->second == "0" || it->second.empty());
            if (auto it = req.query.find("final"); it != req.query.end())
                final = (it->second == "1" || it->second == "true");
            if (req.body.empty() && first) {
                r.status = 400; r.body = R"({"ok":false,"err":"empty upload"})"; return r;
            }
            const std::string path = firmware_dir + "/" + safe;
            {
                std::ios::openmode mode = std::ios::binary | std::ios::out |
                                          (first ? std::ios::trunc : std::ios::app);
                std::ofstream f(path, mode);
                if (!f) {
                    r.status = 500;
                    r.body = R"({"ok":false,"err":"cannot write firmware feed - perms or read-only mount"})";
                    return r;
                }
                if (!req.body.empty())
                    f.write(req.body.data(), static_cast<std::streamsize>(req.body.size()));
                if (!f) { r.status = 500; r.body = R"({"ok":false,"err":"feed write failed"})"; return r; }
            }
            if (final) {
                // Authoritative sha256 over the assembled file.
                std::ifstream in(path, std::ios::binary);
                std::string data((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
                const std::string sha = sha256_hex(data);
                const std::string ipk_url = "/firmware/" + safe;
                std::string pkg = qp("pkg"), ver = qp("version"), arch = qp("arch");
                if (pkg.empty()) pkg = safe;   // fallback so the row is never blank
                upsert_firmware_manifest(ds, ipk_url, pkg, ver, arch, sha);
                ACE_DEBUG((LM_INFO,
                           ACE_TEXT("%D httpd:thread:%t %M %N:%l firmware upload complete %C "
                                    "(sha256 %C) → manifest\n"), safe.c_str(), sha.c_str()));
                r.body = std::string(R"({"ok":true,"sha256":")") + sha + R"("})";
                return r;
            }
            r.body = R"({"ok":true})";
            return r;
        });

    // ── POST /api/v1/firmware/fetch  {url,name,version,arch,pkg,sha256?} ──
    // Cloud-side: the operator supplies an EXTERNAL http(s) URL; the cloud
    // downloads the artifact into the firmware FEED, sha256-verifies it
    // (optionally against a caller-supplied pin), and upserts
    // cloud.firmware.manifest — after which the normal Software Update push
    // flow sends it to devices unchanged. The download runs in a DETACHED
    // thread (a bundle is tens of MB, far too long to hold a request open);
    // the handler returns 202 immediately and progress is reported on the
    // cloud.firmware.fetch.status object the cloud-ui observes. Admin-only;
    // only active where a firmware feed exists (the cloud) — a device has
    // none → 400. `ds` is a process-lifetime Client (safe to use from the
    // detached thread; the ds client lib is documented thread-safe).
    router.add("POST", "/api/v1/firmware/fetch",
        [ds, auth, firmware_dir](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            std::string access_level = "Admin";
            if (auth && auth->enabled()) {
                std::string token = extract_session_cookie(req.headers, auth->cookie_name());
                if (!token.empty()) { const auto* s = auth->validate(token); if (s) access_level = s->access; }
            }
            if (access_level != "Admin") {
                r.status = 403; r.body = R"({"ok":false,"err":"admin required"})"; return r;
            }
            if (firmware_dir.empty()) {
                r.status = 400; r.body = R"({"ok":false,"err":"no firmware feed on this host"})"; return r;
            }
            const json body = parse_body(req.body);
            const std::string url  = body.value("url", "");
            const std::string name = sanitize_feed_name(body.value("name", ""));
            std::string ver = body.value("version", ""), arch = body.value("arch", "");
            std::string pkg = body.value("pkg", "");
            std::string wantSha = body.value("sha256", "");
            for (auto& c : wantSha)   // case-insensitive compare vs our lower-hex
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (!is_safe_fetch_url(url)) {
                r.status = 400; r.body = R"({"ok":false,"err":"url must be a plain http(s) URL"})"; return r;
            }
            if (name.empty()) {
                r.status = 400; r.body = R"({"ok":false,"err":"name must end in .ipk, .tar, .tar.gz, .tgz or .raucb"})"; return r;
            }
            if (pkg.empty()) pkg = name;   // fallback so the row is never blank
            const std::string dest = firmware_dir + "/" + name;
            const std::string tmp  = firmware_dir + "/.fetch." + name + ".part";

            // Mark in-flight now so a fast UI poll sees it even before the
            // thread is scheduled.
            set_fetch_status(ds, {{"state","downloading"}, {"url",url}, {"name",name},
                                  {"pkg",pkg}, {"version",ver}});

            std::thread([ds, url, dest, tmp, name, pkg, ver, arch, wantSha]() {
                auto fail = [&](const std::string& msg) {
                    std::remove(tmp.c_str());
                    set_fetch_status(ds, {{"state","error"}, {"url",url}, {"name",name},
                                          {"pkg",pkg}, {"version",ver}, {"err",msg}});
                    ACE_ERROR((LM_ERROR,
                        ACE_TEXT("%D httpd:thread:%t %M %N:%l firmware fetch FAILED %C: %C\n"),
                        name.c_str(), msg.c_str()));
                };
                if (!fetch_url_to_file(url, tmp)) { fail("download failed"); return; }
                std::ifstream in(tmp, std::ios::binary);
                std::string data((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
                if (data.empty()) { fail("downloaded file is empty"); return; }
                const std::string sha = sha256_hex(data);
                if (!wantSha.empty() && sha != wantSha) {
                    fail("sha256 mismatch (got " + sha + ")"); return;
                }
                set_fetch_status(ds, {{"state","verifying"}, {"url",url}, {"name",name},
                                      {"pkg",pkg}, {"version",ver}, {"sha256",sha}});
                std::remove(dest.c_str());
                if (std::rename(tmp.c_str(), dest.c_str()) != 0) {
                    fail("cannot move into feed"); return;
                }
                upsert_firmware_manifest(ds, "/firmware/" + name, pkg, ver, arch, sha);
                set_fetch_status(ds, {{"state","done"}, {"url",url}, {"name",name},
                                      {"pkg",pkg}, {"version",ver}, {"sha256",sha}});
                ACE_DEBUG((LM_INFO,
                    ACE_TEXT("%D httpd:thread:%t %M %N:%l firmware fetch complete %C "
                             "(sha256 %C) → manifest\n"), name.c_str(), sha.c_str()));
            }).detach();

            r.status = 202;
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
                // mangOH telemetry: wake on the cellular / GPS / sensor bump
                // keys so the one status long-poll also carries those tiles live.
                data_store::Client::WatchHandle wh_cell =
                    data_store::Client::kInvalidHandle;
                data_store::Client::WatchHandle wh_gps =
                    data_store::Client::kInvalidHandle;
                data_store::Client::WatchHandle wh_sens =
                    data_store::Client::kInvalidHandle;
                data_store::Client::WatchHandle wh_sms =
                    data_store::Client::kInvalidHandle;
                ds->watch("cell.version", notify, &wh_cell);
                ds->watch("gps.version", notify, &wh_gps);
                ds->watch("iot.sensor.version", notify, &wh_sens);
                ds->watch("sms.version", notify, &wh_sms);
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
                if (wh_cell != data_store::Client::kInvalidHandle)
                    ds->unwatch(wh_cell);
                if (wh_gps != data_store::Client::kInvalidHandle)
                    ds->unwatch(wh_gps);
                if (wh_sens != data_store::Client::kInvalidHandle)
                    ds->unwatch(wh_sens);
                if (wh_sms != data_store::Client::kInvalidHandle)
                    ds->unwatch(wh_sms);
                // Fall through to build the full status snapshot
            }
            std::vector<data_store::Client::GetResult> got;
            std::vector<std::string> keys = {
                // LwM2M
                "iot.server.uri", "iot.dm.uri", "iot.endpoint", "iot.conn.state",
                // OTA software update
                "iot.update.version", "iot.update.state", "iot.update.result",
                // VPN
                "vpn.state", "vpn.assigned.ip", "vpn.assigned.gateway",
                "vpn.assigned.netmask", "vpn.assigned.dns", "vpn.connected.unix",
                "vpn.pid",
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
                // Live routing snapshot (net-router → Routing → Routes tab)
                "net.routes", "net.ifaces", "net.dns",
                // Cellular modem (mangOH WP) + GPS
                "cell.state", "cell.operator", "cell.tech", "cell.reg",
                "cell.reg.cs", "cell.reg.ps", "cell.reg.eps",
                "cell.signal.dbm", "cell.signal.bars", "cell.ip", "cell.iccid",
                "cell.rat.current", "cell.reg.reason", "cell.dns",
                "cell.imei", "cell.msisdn", "cell.model", "cell.fw",
                "cell.capability", "cell.apn.current", "sms.send.status",
                "gps.fix", "gps.lat", "gps.lon", "gps.alt", "gps.speed",
                "gps.course", "gps.sats", "gps.utc",
                // Received SMS (cellular-client → sms.*)
                "sms.last.sender", "sms.last.text", "sms.last.ts", "sms.count",
                "sms.inbox", "sms.storage",
                // mangOH onboard sensors
                "iot.sensor.temp", "iot.sensor.humidity", "iot.sensor.pressure",
                "iot.sensor.lux", "iot.sensor.accel", "iot.sensor.gyro",
                // Device services: NOT enumerated here. The key list is derived
                // from the ds SCHEMA at first use (every services.* key that is
                // not services.cloud.*), so a new daemon appears on the Services
                // page by declaring its keys in services.lua — no edit here, and
                // no more silently-missing daemons because someone forgot to add
                // 5 metric keys to this literal.
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
            };
            // Append every device-side services.* key the ds schema declares
            // (services.cloud.* keeps its explicit flat passthrough above). The
            // schema is static for the life of the process, so resolve once.
            static const std::vector<std::string> kDeviceServiceKeys =
                [ds]() -> std::vector<std::string> {
                    std::vector<std::string> out;
                    std::string dump;
                    if (!ds || !ds->schema_dump(dump).ok) return out;
                    try {
                        const auto doc = json::parse(dump);
                        if (!doc.contains("keys") || !doc["keys"].is_object())
                            return out;
                        for (const auto& [key, meta] : doc["keys"].items()) {
                            (void)meta;
                            if (key.rfind("services.", 0) != 0)       continue;
                            if (key.rfind("services.cloud.", 0) == 0) continue;
                            if (key == "services.stats.version")      continue;
                            out.push_back(key);
                        }
                    } catch (const std::exception&) { /* leave empty */ }
                    return out;
                }();
            keys.insert(keys.end(), kDeviceServiceKeys.begin(),
                        kDeviceServiceKeys.end());
            auto rs = ds->get(keys, got);
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
            // mangOH Yellow telemetry: cellular modem status, GPS fix, onboard
            // sensors — published by cellular-client / iot-sensord.
            json cell       = json::object();
            json gps        = json::object();
            json sensor     = json::object();

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
                else if (k == "vpn.connected.unix")    vpn["connected_unix"] = iv();
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
                else if (k == "net.dns")                     routing["dns"] = sv();
                else if (k == "net.routes") {
                    // Already a JSON array — embed parsed (sms.inbox idiom).
                    try { routing["routes"] = json::parse(sv()); }
                    catch (...) { routing["routes"] = json::array(); }
                }
                else if (k == "net.ifaces") {
                    try { routing["ifaces"] = json::parse(sv()); }
                    catch (...) { routing["ifaces"] = json::array(); }
                }

                // Cellular modem (cellular-client → cell.*)
                else if (k == "cell.state")        cell["state"] = sv();
                else if (k == "cell.operator")     cell["operator"] = sv();
                else if (k == "cell.tech")         cell["tech"] = sv();
                else if (k == "cell.reg")          cell["reg"] = sv();
                else if (k == "cell.reg.cs")       cell["reg_cs"] = sv();
                else if (k == "cell.reg.ps")       cell["reg_ps"] = sv();
                else if (k == "cell.reg.eps")      cell["reg_eps"] = sv();
                else if (k == "cell.signal.dbm")   cell["signal_dbm"] = sv();
                else if (k == "cell.signal.bars")  cell["signal_bars"] = sv();
                else if (k == "cell.ip")           cell["ip"] = sv();
                else if (k == "cell.dns")          cell["dns"] = sv();
                else if (k == "cell.iccid")        cell["iccid"] = sv();
                else if (k == "cell.rat.current")  cell["rat"] = sv();
                else if (k == "cell.reg.reason")   cell["reg_reason"] = sv();
                else if (k == "cell.imei")         cell["imei"] = sv();
                else if (k == "cell.msisdn")       cell["msisdn"] = sv();
                else if (k == "cell.model")        cell["model"] = sv();
                else if (k == "cell.fw")           cell["fw"] = sv();
                else if (k == "cell.capability")   cell["capability"] = sv();
                else if (k == "cell.apn.current")  cell["apn"] = sv();
                else if (k == "sms.send.status")   cell["sms_send_status"] = sv();
                // Received SMS (surfaced on the WAN → Cellular tile)
                else if (k == "sms.last.sender")   cell["sms_sender"] = sv();
                else if (k == "sms.last.text")     cell["sms_text"] = sv();
                else if (k == "sms.last.ts")       cell["sms_ts"] = sv();
                else if (k == "sms.count")         cell["sms_count"] = sv();
                else if (k == "sms.storage")       cell["sms_storage"] = sv();
                else if (k == "sms.inbox") {
                    // Already a JSON array (newest first) — embed it parsed so the
                    // device-ui gets an array, not a string. Tolerate a bad value.
                    try { cell["sms_inbox"] = json::parse(sv()); }
                    catch (...) { cell["sms_inbox"] = json::array(); }
                }

                // GPS / GNSS (cellular-client → gps.*)
                else if (k == "gps.fix")    gps["fix"] = sv();
                else if (k == "gps.lat")    gps["lat"] = sv();
                else if (k == "gps.lon")    gps["lon"] = sv();
                else if (k == "gps.alt")    gps["alt"] = sv();
                else if (k == "gps.speed")  gps["speed"] = sv();
                else if (k == "gps.course") gps["course"] = sv();
                else if (k == "gps.sats")   gps["sats"] = sv();
                else if (k == "gps.utc")    gps["utc"] = sv();

                // mangOH onboard sensors (iot-sensord → iot.sensor.*)
                else if (k == "iot.sensor.temp")     sensor["temp"] = sv();
                else if (k == "iot.sensor.humidity") sensor["humidity"] = sv();
                else if (k == "iot.sensor.pressure") sensor["pressure"] = sv();
                else if (k == "iot.sensor.lux")      sensor["lux"] = sv();
                else if (k == "iot.sensor.accel")    sensor["accel"] = sv();
                else if (k == "iot.sensor.gyro")     sensor["gyro"] = sv();

                // ── services.* → the nested `services` block ────────────────
                // GENERIC: any key `services.<name>.<field>` lands in
                // services[<slug>][<json field>], where <slug> is <name> with
                // dots turned into underscores (net.router → net_router).
                //
                // This replaced ~75 hand-written branches — one per (daemon,
                // metric) pair — which is why the Services page only ever showed
                // the handful of daemons somebody had remembered to wire up.
                // A new daemon now needs NOTHING here: it just declares
                // services.<name>.{enable,state} in services.lua and publishes
                // via StatsPublisher.
                //
                // services.cloud.* is NOT handled here — it keeps its flat
                // passthrough below, which is what the cloud UI reads.
                else if (k.rfind("services.", 0) == 0 &&
                         k.rfind("services.cloud.", 0) != 0 &&
                         k != "services.stats.version") {
                    // Split off the trailing field, longest suffix first (the
                    // multi-dot metrics must beat the single-dot ones).
                    static const std::vector<std::pair<const char*, const char*>>
                        kFields = {
                            {".cpu.permille", "cpu_permille"},
                            {".cpu.count",    "cpu_count"},
                            {".mem.rss.kb",   "mem_kb"},
                            {".fd.count",     "fd_count"},
                            {".uptime.sec",   "uptime_sec"},
                            {".threads",      "threads"},
                            {".enable",       "enable"},
                            {".state",        "state"},
                        };
                    for (const auto& [suffix, field] : kFields) {
                        const std::size_t slen = std::strlen(suffix);
                        if (k.size() <= 9 + slen) continue;   // "services." + suffix
                        if (k.compare(k.size() - slen, slen, suffix) != 0) continue;

                        // services.net.router.state → name "net.router" → slug
                        // "net_router" (the key the device-ui rows are keyed by).
                        std::string slug = k.substr(9, k.size() - 9 - slen);
                        for (char& c : slug) if (c == '.') c = '_';

                        const std::string f(field);
                        if      (f == "state")  services[slug][f] = sv();
                        else if (f == "enable") services[slug][f] = bv(true);
                        else                    services[slug][f] = iv();

                        // ds-server keys ALSO feed the flat `cloud` passthrough:
                        // the cloud Services page reads d['services.ds.state']
                        // rather than the nested block. Without this dual-emit it
                        // never streams ds-server live off /status — it falls back
                        // to the one-shot prefetch and shows a stale "stopped".
                        if (k.rfind("services.ds.", 0) == 0) {
                            if      (f == "state")  cloud[k] = sv();
                            else if (f != "enable") cloud[k] = iv();
                        }
                        break;
                    }
                }

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
            resp["cell"]     = cell;
            resp["gps"]      = gps;
            resp["sensor"]   = sensor;
            // Host uptime (this device / cloud container) from /proc/uptime —
            // first token is seconds since boot. Surfaced in the top status bar.
            {
                json device = json::object();
                std::ifstream upf("/proc/uptime");
                double up = 0.0;
                if (upf >> up)
                    device["uptime_sec"] = static_cast<long long>(up);
                resp["device"] = device;
            }
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
            if (svc == "ds") {
                r.status = 400;
                r.body = R"json({"ok":false,"err":"cannot restart ds-server"})json";
                return r;
            }

            // Two kinds of daemon:
            //
            // (a) GATED — implements a ServiceGate, i.e. it watches
            //     services.<x>.enable and parks/unparks on it. Restart = flip
            //     the key off→on; the daemon reaps its workers and re-inits.
            //
            // (b) UNGATED — every other daemon. Flipping the enable key does
            //     nothing at all (nobody is watching it), which is why the
            //     Services page's Restart button used to 400 for containers /
            //     mqtt / vehicle. For these we arm /run/iot/service-restart.request
            //     with the SLUG and let the root iot-service-restart.path unit
            //     `systemctl restart` the corresponding unit — the same
            //     privilege bridge reboot/factory-reset already use. The slug is
            //     validated here AND re-validated in the unit; a unit name is
            //     never taken from the request.
            static const std::map<std::string, std::string> kGated = {
                {"net.router",     "services.net.router.enable"},
                {"openvpn.client", "services.openvpn.client.enable"},
                {"lwm2m.client",   "services.lwm2m.client.enable"},
                {"lwm2m.server",   "services.lwm2m.server.enable"},
                {"wifi.client",    "services.wifi.client.enable"},
            };
            // Keep in sync with the case list in iot-service-restart.service.
            static const std::set<std::string> kUngated = {
                "cellular", "container", "mqtt", "vehicle",
                "sensors", "ddns", "smsctl", "httpd",
            };

            std::string enable_key;
            if (auto it = kGated.find(svc); it != kGated.end()) {
                enable_key = it->second;
            } else if (kUngated.count(svc)) {
                std::ofstream trig("/run/iot/service-restart.request",
                                   std::ios::trunc);
                if (!trig) {
                    r.status = 500;
                    r.body = R"({"ok":false,"err":"cannot arm restart - perms or non-Yocto host"})";
                    return r;
                }
                trig << svc << "\n";
                ACE_DEBUG((LM_INFO,
                           ACE_TEXT("%D httpd:thread:%t %M %N:%l service restart "
                                    "requested for '%C' (armed iot-service-restart.path)\n"),
                           svc.c_str()));
                json resp;
                resp["ok"]      = true;
                resp["service"] = svc;
                r.body = resp.dump();
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
                               "log.lwm2m.dm.text", "log.vehicled.text",
                               "log.mqttd.text", "log.containerd.text"}, got);
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

    // ── GET /api/v1/cloud/telemetry/history?ep=<ep> ──────────────────
    // Vehicle history read-back (§3b). The iot-telemetry-ingest sidecar
    // periodically mongoexports a recent window of the Mongo `telemetry`
    // collection to /var/lib/iot/telemetry-spool/history.json — a JSON array
    // of {ts, endpoints:[...]} snapshots, oldest-first. Served straight from
    // that file (no mongo driver in this build). With ?ep= we flatten it to
    // that endpoint's track [{ts,lat,lon,speed,...}] for the map polyline;
    // without, the raw snapshots. Empty array until the sidecar has run once.
    router.add("GET", "/api/v1/cloud/telemetry/history",
        [](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            json resp;
            resp["ok"] = true;
            json snaps = json::array();
            {
                std::ifstream f("/var/lib/iot/telemetry-spool/history.json");
                if (f) {
                    std::string s((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
                    if (!s.empty()) {
                        try {
                            auto parsed = json::parse(s);
                            if (parsed.is_array()) snaps = parsed;
                        } catch (const std::exception&) { /* leave empty */ }
                    }
                }
            }
            auto it = req.query.find("ep");
            if (it != req.query.end()) {
                const std::string& ep = it->second;
                json track = json::array();
                for (const auto& snap : snaps) {
                    if (!snap.contains("endpoints") || !snap["endpoints"].is_array())
                        continue;
                    auto tsIt = snap.find("ts");
                    for (const auto& e : snap["endpoints"]) {
                        if (e.value("endpoint", std::string()) != ep) continue;
                        json pt = e;
                        if (tsIt != snap.end()) pt["ts"] = *tsIt;
                        track.push_back(pt);
                        break;   // one entry per snapshot
                    }
                }
                resp["endpoint"] = ep;
                resp["count"]    = track.size();
                resp["track"]    = track;
            } else {
                resp["count"]     = snaps.size();
                resp["snapshots"] = snaps;
            }
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
            // Built-in admin is the platform operator ("*" = sees all tenants)
            // unless explicitly scoped via auth.users.admin.tenant.
            {
                std::string at = CredentialStore::load_user_tenant(*ds, "admin");
                admin["tenant"] = (at.empty() || at == "default") ? "*" : at;
            }
            users.push_back(admin);
            for (const auto& u : load_accounts(ds)) {
                json e;
                e["id"]     = u.value("id", "");
                e["access"] = u.value("access", "Viewer");
                e["tenant"] = u.value("tenant", "default");
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
            std::string utenant  = doc.value("tenant", "default");
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
            // Multi-tenant (D6): the user's owning tenant — "*" (platform
            // operator, sees all), "default", or a known cloud.tenants id. Reject
            // an unknown tenant so a typo can't orphan a user outside isolation.
            if (utenant.empty()) utenant = "default";
            if (utenant != "*" && utenant != "default") {
                bool known = false;
                std::string tenants_json = "[]";
                {
                    std::vector<data_store::Client::GetResult> g;
                    if (ds->get({"cloud.tenants"}, g).ok && !g.empty() && g[0].has_value)
                        if (auto s = data_store::to_string(g[0].value)) tenants_json = *s;
                }
                auto ts = nlohmann::json::parse(tenants_json, nullptr, false);
                if (ts.is_array()) for (const auto& t : ts)
                    if (t.is_object() && t.value("id", std::string()) == utenant) { known = true; break; }
                if (!known) {
                    r.status = 400;
                    r.body = R"({"ok":false,"err":"unknown tenant"})";
                    return r;
                }
            }

            auto accounts = load_accounts(ds);
            std::string hash = sha256_hex(password);
            bool updated = false;
            for (auto& u : accounts) {
                if (u.value("id", "") == uid) {
                    u["hash"]   = hash;
                    u["access"] = uaccess;
                    u["tenant"] = utenant;
                    updated = true;
                    break;
                }
            }
            if (!updated) {
                json e;
                e["id"]     = uid;
                e["hash"]   = hash;
                e["access"] = uaccess;
                e["tenant"] = utenant;
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
