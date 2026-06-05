/// L18/D4 — API handler integration tests.
///
/// Spawn ds-server as a subprocess (same pattern as
/// service_gate_test.cpp), install handlers, exercise
/// /api/v1/db/get, /api/v1/db/set, and long-poll.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "data_store/client.hpp"
#include "data_store/value.hpp"
#include "handler.hpp"
#include "parser.hpp"
#include "router.hpp"

using http_server::HttpParser;
using http_server::HttpResponse;
using http_server::Router;

namespace ds = data_store;

namespace {

std::string make_temp_dir() {
    char tpl[64] = "/tmp/httph-XXXXXX";
    if (char* d = mkdtemp(tpl)) return d;
    ::mkdir("/tmp", 01777);
    std::strcpy(tpl, "/tmp/httph-XXXXXX");
    if (char* d = mkdtemp(tpl)) return d;
    return {};
}

std::string find_ds_server() {
    // Check cmake build output first (when run via ctest),
    // then common dev paths, then installed path.
    for (auto p : {
            "./data-store/ds-server",          // ctest from build dir
            "../data-store/ds-server",
            "./ds-server",
            "../ds-server",
            "modules/data-store/build/ds-server",
            "/usr/local/bin/ds-server"}) {
        struct ::stat st;
        if (::stat(p, &st) == 0 && (st.st_mode & S_IXUSR)) return p;
    }
    return {};
}

std::string find_services_lua() {
    for (auto p : {
            "/src/modules/data-store/schemas/services.lua",
            "../data-store/../schemas/services.lua",
            "../schemas/services.lua",
            "../../schemas/services.lua",
            "schemas/services.lua",
            "modules/data-store/schemas/services.lua"}) {
        std::ifstream in(p);
        if (in.good()) return p;
    }
    return {};
}

class HandlerFixture {
public:
    HandlerFixture() {
        m_dir = make_temp_dir();
        if (m_dir.empty()) return;
        m_server_bin = find_ds_server();
        if (m_server_bin.empty()) return;
        auto sl = find_services_lua();
        if (sl.empty()) return;

        m_schema_dir = m_dir + "/schemas";
        ::mkdir(m_schema_dir.c_str(), 0755);
        std::ifstream in(sl);
        std::ofstream out(m_schema_dir + "/services.lua");
        out << in.rdbuf();

        m_sock  = m_dir + "/ds.sock";
        m_store = m_dir + "/store.lua";

        m_server_pid = ::fork();
        if (m_server_pid == 0) {
            ::execlp(m_server_bin.c_str(), m_server_bin.c_str(),
                     ("ds-socket=" + m_sock).c_str(),
                     ("ds-store=" + m_store).c_str(),
                     ("ds-schema-dir=" + m_schema_dir).c_str(),
                     nullptr);
            ::_exit(127);
        }

        for (int i = 0; i < 100; ++i) {
            struct ::stat st;
            if (::stat(m_sock.c_str(), &st) == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        auto cs = m_client.connect(m_sock);
        if (!cs.ok) return;

        install_handlers(m_router, &m_client, nullptr);
        m_ok = true;
    }

    ~HandlerFixture() {
        m_client.close();
        if (m_server_pid > 0) {
            ::kill(m_server_pid, SIGTERM);
            int st = 0;
            for (int i = 0; i < 20; ++i) {
                if (::waitpid(m_server_pid, &st, WNOHANG) == m_server_pid) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            ::kill(m_server_pid, SIGKILL);
            ::waitpid(m_server_pid, &st, 0);
        }
    }

    bool ok() const { return m_ok; }
    Router& router() { return m_router; }
    ds::Client& client() { return m_client; }

    HttpResponse dispatch(const std::string& method,
                          const std::string& path,
                          const std::string& body = {}) {
        HttpParser::Request req;
        req.method = method;
        req.path   = path;
        req.body   = body;
        return m_router.route(req);
    }

private:
    std::string  m_dir, m_server_bin, m_schema_dir, m_sock, m_store;
    pid_t        m_server_pid = -1;
    ds::Client   m_client;
    Router       m_router;
    bool         m_ok = false;
};

} // namespace

// ─────── D4-T1: DbGet returns values ──────────────────────────

TEST(Handler, DbGet_returns_values) {
    HandlerFixture f;
    if (!f.ok()) GTEST_SKIP() << "ds-server unavailable";

    // Seed a key
    f.client().set("iot.endpoint", std::string("test-1"));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto resp = f.dispatch("POST", "/api/v1/db/get",
                           R"({"keys":["iot.endpoint"]})");
    EXPECT_EQ(200, resp.status);
    EXPECT_NE(std::string::npos, resp.body.find("test-1"));
    EXPECT_NE(std::string::npos, resp.body.find("\"ok\":true"));
}

// ─────── D4-T2: DbGet missing key returns null ────────────────

TEST(Handler, DbGet_missing_key_returns_null) {
    HandlerFixture f;
    if (!f.ok()) GTEST_SKIP();

    auto resp = f.dispatch("POST", "/api/v1/db/get",
                           R"({"keys":["nonexistent.key"]})");
    EXPECT_EQ(200, resp.status);
    EXPECT_NE(std::string::npos, resp.body.find("null"));
}

// ─────── D4-T3: DbSet writes and returns changed count ─────────

TEST(Handler, DbSet_writes_and_returns_changed_count) {
    HandlerFixture f;
    if (!f.ok()) GTEST_SKIP();

    auto resp = f.dispatch("POST", "/api/v1/db/set",
        R"({"pairs":[{"key":"iot.lifetime","value":3600},{"key":"iot.binding","value":"UDP"}]})");
    EXPECT_EQ(200, resp.status);
    EXPECT_NE(std::string::npos, resp.body.find("\"changed\":2"));
}

// ─────── D4-T4: DbSet schema rejection ────────────────────────

TEST(Handler, DbSet_schema_rejection) {
    HandlerFixture f;
    if (!f.ok()) GTEST_SKIP();

    // services.ds.enable is deliberately absent from the schema
    auto resp = f.dispatch("POST", "/api/v1/db/set",
        R"({"pairs":[{"key":"services.ds.enable","value":false}]})");
    EXPECT_EQ(400, resp.status);
    EXPECT_NE(std::string::npos, resp.body.find("services.ds.enable"));
}

// ─────── D4-T5: LongPoll immediate return ─────────────────────

TEST(Handler, LongPoll_immediate_return) {
    HandlerFixture f;
    if (!f.ok()) GTEST_SKIP();

    f.client().set("iot.endpoint", std::string("test-lp"));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    HttpParser::Request req;
    req.method = "GET";
    req.path   = "/api/v1/db/get";
    req.query["key"] = "iot.endpoint";
    // no timeout → immediate

    auto resp = f.router().route(req);
    EXPECT_EQ(200, resp.status);
    EXPECT_NE(std::string::npos, resp.body.find("\"changed\":false"));
}

// ─────── D4-T6: LongPoll waits for change ─────────────────────

TEST(Handler, LongPoll_waits_for_change) {
    HandlerFixture f;
    if (!f.ok()) GTEST_SKIP();

    f.client().set("iot.endpoint", std::string("before"));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Start long poll in background (timeout=3s)
    auto fut = std::async(std::launch::async, [&]() {
        HttpParser::Request req;
        req.method = "GET";
        req.path   = "/api/v1/db/get";
        req.query["key"]     = "iot.endpoint";
        req.query["timeout"] = "3";
        return f.router().route(req);
    });

    // Wait a tick for the poll to register
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Change the key
    f.client().set("iot.endpoint", std::string("after"));

    auto resp = fut.get();
    EXPECT_EQ(200, resp.status);
    EXPECT_NE(std::string::npos, resp.body.find("\"changed\":true"));
    EXPECT_NE(std::string::npos, resp.body.find("after"));
}
