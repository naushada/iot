#include "http_server/handler.hpp"

#include "data_store/client.hpp"
#include "data_store/value.hpp"

#include <nlohmann/json.hpp>

#include <ace/Log_Msg.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

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

} // namespace

void install_handlers(Router& router, data_store::Client* ds) {
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
        [ds](const HttpParser::Request& req) -> HttpResponse {
            HttpResponse r;
            if (!ds) {
                r.status = 500;
                r.body = R"({"ok":false,"err":"data store not connected"})";
                return r;
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
}

} // namespace http_server
