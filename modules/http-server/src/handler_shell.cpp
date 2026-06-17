#include "auth.hpp"
#include "router.hpp"
#include "shell.hpp"

#include "data_store/client.hpp"
#include "data_store/value.hpp"

#include <nlohmann/json.hpp>

#include <ace/Log_Msg.h>

#include <cstdlib>
#include <string>

namespace http_server {

namespace {

using json = nlohmann::json;

json parse_body(const std::string& body) {
    if (body.empty()) return json::object();
    try { return json::parse(body); }
    catch (const std::exception&) { return json::object(); }
}

/// Master switch: http.shell.enabled (default false). Read per request so
/// flipping it off kills new sessions at once.
bool shell_enabled(data_store::Client* ds) {
    if (!ds) return false;
    std::vector<data_store::Client::GetResult> got;
    auto rs = ds->get({"http.shell.enabled"}, got);
    if (rs.ok && !got.empty() && got[0].has_value) {
        if (auto b = data_store::to_bool(got[0].value)) return *b;
    }
    return false;
}

/// Resolve the caller's access level from the session cookie. Mirrors the
/// /api/v1/update/upload gate: absent auth → "Admin" (auth disabled = open).
std::string access_of(SessionStore* auth, const HttpParser::Request& req) {
    if (!auth || !auth->enabled()) return "Admin";
    std::string token = extract_session_cookie(req.headers, auth->cookie_name());
    if (token.empty()) return "Viewer";
    const auto* s = auth->validate(token);
    return s ? s->access : "Viewer";
}

/// Common gate for every /shell/* route: feature flag + Admin. On failure
/// fills `r` (403/503) and returns false.
bool gate(data_store::Client* ds, SessionStore* auth,
          const HttpParser::Request& req, HttpResponse& r) {
    if (!shell_enabled(ds)) {
        r.status = 403;
        r.body = R"json({"ok":false,"err":"shell disabled (http.shell.enabled)"})json";
        return false;
    }
    if (access_of(auth, req) != "Admin") {
        r.status = 403;
        r.body = R"({"ok":false,"err":"admin required"})";
        return false;
    }
    return true;
}

int clamp_int(const HttpParser::Request& req, const char* key, int def,
              int lo, int hi) {
    auto it = req.query.find(key);
    if (it == req.query.end()) return def;
    int v = std::atoi(it->second.c_str());
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

} // namespace

void install_shell_handlers(Router& router, data_store::Client* ds,
                            SessionStore* auth, ShellManager* mgr) {
    if (!mgr) return;

    // ── POST /api/v1/shell/open  { cols, rows } → { ok, sid } ─────────
    router.add("POST", "/api/v1/shell/open",
        [ds, auth, mgr](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            if (!gate(ds, auth, req, r)) return r;
            // Pick up operator changes to the tunables without a restart.
            if (ds) {
                std::vector<data_store::Client::GetResult> got;
                auto rs = ds->get({"http.shell.idle.sec",
                                   "http.shell.max.sessions"}, got);
                if (rs.ok) {
                    for (const auto& g : got) {
                        if (!g.has_value) continue;
                        if (auto n = data_store::to_int32(g.value)) {
                            if (g.key == "http.shell.idle.sec")
                                mgr->set_idle_sec(*n);
                            else if (g.key == "http.shell.max.sessions" && *n > 0)
                                mgr->set_max_sessions(
                                    static_cast<std::size_t>(*n));
                        } else if (auto u = data_store::to_uint32(g.value)) {
                            if (g.key == "http.shell.idle.sec")
                                mgr->set_idle_sec(static_cast<int>(*u));
                            else if (g.key == "http.shell.max.sessions" && *u > 0)
                                mgr->set_max_sessions(
                                    static_cast<std::size_t>(*u));
                        }
                    }
                }
            }
            auto doc = parse_body(req.body);
            unsigned short cols =
                static_cast<unsigned short>(doc.value("cols", 80));
            unsigned short rows =
                static_cast<unsigned short>(doc.value("rows", 24));
            std::string err;
            std::string sid = mgr->open(cols, rows, err);
            json resp;
            if (sid.empty()) {
                r.status = 503;
                resp["ok"]  = false;
                resp["err"] = err.empty() ? "failed to open shell" : err;
            } else {
                resp["ok"]  = true;
                resp["sid"] = sid;
            }
            r.body = resp.dump();
            return r;
        });

    // ── GET /api/v1/shell/output?sid=&timeout=N (long-poll) ──────────
    // Returns base64 PTY output; blocks up to timeout (cap 30s, well under
    // the cloud proxy's 75s recv window) or until bytes arrive. `closed`
    // signals the child exited — the UI then stops polling.
    router.add("GET", "/api/v1/shell/output",
        [ds, auth, mgr](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            if (!gate(ds, auth, req, r)) return r;
            auto it = req.query.find("sid");
            if (it == req.query.end()) {
                r.status = 400;
                r.body = R"({"ok":false,"err":"missing 'sid'"})";
                return r;
            }
            const std::string sid = it->second;
            int timeout = clamp_int(req, "timeout", 25, 0, 30);
            auto sess = mgr->find(sid);
            json resp;
            if (!sess) {
                resp["ok"]     = true;   // not an error — session just gone
                resp["sid"]    = sid;
                resp["data"]   = "";
                resp["closed"] = true;
                r.body = resp.dump();
                return r;
            }
            bool closed = false;
            std::string out = sess->read_output(timeout, closed);
            if (closed) mgr->close(sid);  // reap on EOF
            resp["ok"]     = true;
            resp["sid"]    = sid;
            resp["data"]   = base64_encode(out);
            resp["closed"] = closed;
            r.body = resp.dump();
            return r;
        });

    // ── POST /api/v1/shell/input  { sid, data(base64) } ──────────────
    router.add("POST", "/api/v1/shell/input",
        [ds, auth, mgr](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            if (!gate(ds, auth, req, r)) return r;
            auto doc = parse_body(req.body);
            std::string sid = doc.value("sid", "");
            auto sess = sid.empty() ? nullptr : mgr->find(sid);
            json resp;
            if (!sess) {
                r.status = 404;
                resp["ok"]  = false;
                resp["err"] = "no such session";
                r.body = resp.dump();
                return r;
            }
            std::string data = base64_decode(doc.value("data", ""));
            bool ok = data.empty() ? true : sess->write_input(data);
            resp["ok"] = ok;
            if (!ok) { resp["err"] = "write failed"; r.status = 500; }
            r.body = resp.dump();
            return r;
        });

    // ── POST /api/v1/shell/resize  { sid, cols, rows } ───────────────
    router.add("POST", "/api/v1/shell/resize",
        [ds, auth, mgr](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            if (!gate(ds, auth, req, r)) return r;
            auto doc = parse_body(req.body);
            std::string sid = doc.value("sid", "");
            auto sess = sid.empty() ? nullptr : mgr->find(sid);
            json resp;
            if (!sess) {
                r.status = 404;
                resp["ok"]  = false;
                resp["err"] = "no such session";
                r.body = resp.dump();
                return r;
            }
            sess->resize(static_cast<unsigned short>(doc.value("cols", 80)),
                         static_cast<unsigned short>(doc.value("rows", 24)));
            resp["ok"] = true;
            r.body = resp.dump();
            return r;
        });

    // ── POST /api/v1/shell/close  { sid } ────────────────────────────
    router.add("POST", "/api/v1/shell/close",
        [ds, auth, mgr](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            if (!gate(ds, auth, req, r)) return r;
            auto doc = parse_body(req.body);
            std::string sid = doc.value("sid", "");
            if (!sid.empty()) mgr->close(sid);
            r.body = R"({"ok":true})";
            return r;
        });
}

} // namespace http_server
