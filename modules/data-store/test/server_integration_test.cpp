/// D1 risk-gate integration tests: spin up a real Server in-process,
/// open a real Client, prove the welcome round-trip works for one
/// session AND for 16 concurrent sessions (REQ-DS-011).
///
/// All sockets live under a per-test mkdtemp dir so parallel test
/// runs don't fight.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <grp.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <ace/Reactor.h>
#include <ace/Time_Value.h>

#include "data_store/client.hpp"
#include "data_store/proto.hpp"
#include "../src/server/data_store.hpp"
#include "../src/server/schema.hpp"
#include "../src/server/server.hpp"
#include "../src/server/worker_pool.hpp"

namespace ds = data_store;

namespace {

/// Create a per-test scratch directory under /tmp (auto-creating /tmp
/// if needed — mirrors apps/test/objects_test.cpp::make_temp_dir).
std::string make_temp_dir() {
    char dirTemplate[64] = "/tmp/ds-test-XXXXXX";
    if (char* d = mkdtemp(dirTemplate)) return d;
    ::mkdir("/tmp", 01777);
    std::strcpy(dirTemplate, "/tmp/ds-test-XXXXXX");
    if (char* d = mkdtemp(dirTemplate)) return d;
    std::strcpy(dirTemplate, "./ds-test-XXXXXX");
    if (char* d = mkdtemp(dirTemplate)) return d;
    return {};
}

/// One reactor tick on the test thread — services pending accepts
/// without spinning the whole event loop.
void pump(int times = 4) {
    ACE_Time_Value tv(0, 100 * 1000);   // 100 ms
    for (int i = 0; i < times; ++i) {
        ACE_Reactor::instance()->handle_events(tv);
    }
}

} // namespace

/* ────────────────────────────── REQ-DS-011 ───────────────────────── */

TEST(ServerIntegration, DS_REQ_DS_011_one_client_connects_and_gets_response) {
    std::string dir = make_temp_dir();
    if (dir.empty()) GTEST_SKIP() << "no writable /tmp or cwd";
    std::string sock = dir + "/ds.sock";

    auto store = std::make_shared<ds::server::DataStore>();
    ds::server::WorkerPool pool(store);
    ASSERT_EQ(0, pool.open());
    ds::server::Server server(store, &pool, sock);
    ASSERT_EQ(0, server.open());

    // EMP has no welcome — to prove the wire works, issue a get on a
    // missing key and assert ok+empty data.
    auto fut = std::async(std::launch::async, [&]() {
        ds::Client cli;
        auto cs = cli.connect(sock);
        if (!cs.ok) return std::string("CONNECT_FAILED:") + cs.err;
        std::vector<ds::Client::GetResult> got;
        auto rs = cli.get({"nope"}, got, /*timeout_ms=*/2000);
        if (!rs.ok) return std::string("GET_FAILED:") + rs.err;
        if (got.size() != 1) return std::string("BAD_SIZE");
        if (got[0].has_value) return std::string("UNEXPECTED_VALUE");
        return std::string("ok");
    });

    pump(20);                       // service the connect
    std::string got = fut.get();

    EXPECT_EQ("ok", got);
    server.close();
    pool.close();
    ::unlink(sock.c_str());
    ::rmdir(dir.c_str());
}

TEST(ServerIntegration, DS_REQ_DS_011_concurrent_sessions) {
    std::string dir = make_temp_dir();
    if (dir.empty()) GTEST_SKIP() << "no writable /tmp or cwd";
    std::string sock = dir + "/ds.sock";

    auto store = std::make_shared<ds::server::DataStore>();
    ds::server::WorkerPool pool(store);
    ASSERT_EQ(0, pool.open());
    ds::server::Server server(store, &pool, sock);
    ASSERT_EQ(0, server.open());

    constexpr int N = 16;
    std::vector<std::future<std::string>> clients;
    clients.reserve(N);
    for (int i = 0; i < N; ++i) {
        clients.emplace_back(std::async(std::launch::async, [&, i]() {
            // Tiny stagger so the 16 connects land within a few
            // reactor wake-ups, not exactly at once.
            std::this_thread::sleep_for(
                std::chrono::milliseconds(5 + (i % 4)));
            ds::Client cli;
            auto cs = cli.connect(sock);
            if (!cs.ok) return std::string("CONNECT:") + cs.err;
            // Each session does one Get to prove the wire is alive.
            std::vector<ds::Client::GetResult> got;
            auto rs = cli.get({"nope"}, got, 3000);
            if (!rs.ok) return std::string("GET:") + rs.err;
            return std::string("ok");
        }));
    }

    // Drain accepts. 16 short connections complete inside ~200 ms on
    // a healthy box; pump a few hundred ms of reactor ticks.
    pump(60);

    int ok = 0;
    for (auto& f : clients) {
        std::string got = f.get();
        if (got == "ok") ++ok;
        else ADD_FAILURE() << "session result: " << got;
    }
    EXPECT_EQ(N, ok);

    server.close();
    pool.close();
    ::unlink(sock.c_str());
    ::rmdir(dir.c_str());
}

/* ──────────────── PSK provisioning: read_acl (task C4) ───────────────
 *
 * End-to-end through Server → Worker. The test process runs as its own
 * uid (root in the CI container). The PSK key declares read_acl for a
 * uid that is NOT the test's, so a Get must be denied — proving write-only
 * — while a non-protected key in the same namespace stays readable. Then
 * flipping iot.dev.mode=true must lift the block (commissioning bypass).
 */

namespace {
std::shared_ptr<ds::server::SchemaRegistry>
make_psk_schema(const std::string& dir) {
    // read_acl names uid 999999 (not the test process) so reads are
    // denied; write_acl is left open so the test can seed the value.
    std::ofstream(dir + "/iot.lua") << R"(
return {
  namespace = "iot",
  keys = {
    ["iot.bs.psk.key"] = { type="opaque", read_acl={"uid:999999"} },
    ["iot.endpoint"]   = { type="string", default="urn:dev:client-1" },
    ["iot.dev.mode"]   = { type="boolean", default=false },
  },
})";
    auto schema = std::make_shared<ds::server::SchemaRegistry>();
    schema->load_directory(dir);
    return schema;
}
} // namespace

TEST(ServerIntegration, PSK_read_acl_blocks_get_until_dev_mode) {
    std::string dir = make_temp_dir();
    if (dir.empty()) GTEST_SKIP() << "no writable /tmp or cwd";
    std::string sock = dir + "/ds.sock";

    auto schema = make_psk_schema(dir);
    auto store = std::make_shared<ds::server::DataStore>();
    ds::server::WorkerPool pool(store, schema);
    ASSERT_EQ(0, pool.open());
    ds::server::Server server(store, &pool, sock);
    ASSERT_EQ(0, server.open());

    auto fut = std::async(std::launch::async, [&]() -> std::string {
        ds::Client cli;
        auto cs = cli.connect(sock);
        if (!cs.ok) return "CONNECT:" + cs.err;

        // Seed the secret (write_acl open).
        auto ss = cli.set({{"iot.bs.psk.key",
                            ds::Value{std::string("deadbeef")}}}, 2000);
        if (!ss.ok) return "SET_PSK:" + ss.err;

        // A non-protected key is readable.
        std::vector<ds::Client::GetResult> g1;
        auto r1 = cli.get({"iot.endpoint"}, g1, 2000);
        if (!r1.ok) return "GET_EP:" + r1.err;

        // The protected key must be DENIED (write-only).
        std::vector<ds::Client::GetResult> g2;
        auto r2 = cli.get({"iot.bs.psk.key"}, g2, 2000);
        if (r2.ok) return "EXPECTED_DENY_BUT_OK";

        // Enable dev-mode → the same Get now succeeds.
        auto sd = cli.set({{"iot.dev.mode", ds::Value{true}}}, 2000);
        if (!sd.ok) return "SET_DEV:" + sd.err;
        std::vector<ds::Client::GetResult> g3;
        auto r3 = cli.get({"iot.bs.psk.key"}, g3, 2000);
        if (!r3.ok) return "GET_DEV:" + r3.err;
        if (g3.size() != 1 || !g3[0].has_value) return "DEV_NO_VALUE";
        if (std::get<std::string>(g3[0].value) != "deadbeef")
            return "DEV_WRONG_VALUE";
        return "ok";
    });

    pump(40);
    EXPECT_EQ("ok", fut.get());

    server.close();
    pool.close();
    ::unlink(sock.c_str());
    ::unlink((dir + "/iot.lua").c_str());
    ::rmdir(dir.c_str());
}

TEST(ServerIntegration, PSK_socket_group_is_applied) {
    // Task J3: ds-server chgrp's the socket to a shared group so a
    // static engineer account can reach it. Use a well-known group the
    // (root) test process can chgrp to; assert the socket's gid matches.
    struct group* gr = ::getgrnam("daemon");
    if (!gr) gr = ::getgrnam("bin");
    if (!gr) GTEST_SKIP() << "no well-known group to test chgrp";

    std::string dir = make_temp_dir();
    if (dir.empty()) GTEST_SKIP() << "no writable /tmp or cwd";
    std::string sock = dir + "/ds.sock";

    auto store = std::make_shared<ds::server::DataStore>();
    ds::server::WorkerPool pool(store);
    ASSERT_EQ(0, pool.open());
    ds::server::Server server(store, &pool, sock);
    server.set_socket_group("daemon");
    ASSERT_EQ(0, server.open());

    struct stat st{};
    ASSERT_EQ(0, ::stat(sock.c_str(), &st)) << "stat errno=" << errno;
    if (::getgrnam("daemon"))
        EXPECT_EQ(::getgrnam("daemon")->gr_gid, st.st_gid);
    EXPECT_EQ(0660u, st.st_mode & 0777u);

    server.close();
    pool.close();
    ::unlink(sock.c_str());
    ::rmdir(dir.c_str());
}

/* ────────────────────────────── NFR-DS-004 ───────────────────────── */

TEST(ServerIntegration, DS_NFR_DS_004_socket_mode_is_0660) {
    std::string dir = make_temp_dir();
    if (dir.empty()) GTEST_SKIP() << "no writable /tmp or cwd";
    std::string sock = dir + "/ds.sock";

    auto store = std::make_shared<ds::server::DataStore>();
    ds::server::WorkerPool pool(store);
    ASSERT_EQ(0, pool.open());
    ds::server::Server server(store, &pool, sock);
    ASSERT_EQ(0, server.open());

    struct stat st{};
    ASSERT_EQ(0, ::stat(sock.c_str(), &st)) << "stat failed errno=" << errno;
    // Compare only the permission bits (mask away the S_IFSOCK type).
    EXPECT_EQ(0660u, st.st_mode & 0777u)
        << "got mode 0" << std::oct << (st.st_mode & 0777u);

    server.close();
    pool.close();
    ::unlink(sock.c_str());
    ::rmdir(dir.c_str());
}
