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
#include <future>
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

TEST(ServerIntegration, DS_REQ_DS_011_one_client_gets_welcome) {
    std::string dir = make_temp_dir();
    if (dir.empty()) GTEST_SKIP() << "no writable /tmp or cwd";
    std::string sock = dir + "/ds.sock";

    auto store = std::make_shared<ds::server::DataStore>();
    ds::server::WorkerPool pool(store);
    ASSERT_EQ(0, pool.open());
    ds::server::Server server(store, &pool, sock);
    ASSERT_EQ(0, server.open());

    // Connect from the test thread; the reactor pump below services
    // the accept. We give the client a generous timeout in case the
    // first pump tick lands after recv_welcome blocks.
    auto fut = std::async(std::launch::async, [&]() {
        ds::Client cli;
        auto cs = cli.connect(sock);
        if (!cs.ok) return std::string("CONNECT_FAILED:") + cs.err;
        std::string w;
        auto rs = cli.recv_welcome(w, /*timeout_ms=*/2000);
        if (!rs.ok) return std::string("RECV_FAILED:") + rs.err;
        return w;
    });

    pump(20);                       // service the connect
    std::string got = fut.get();

    EXPECT_EQ(std::string(ds::proto::kWelcomeLine), got);
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
            std::string w;
            auto rs = cli.recv_welcome(w, 3000);
            if (!rs.ok) return std::string("RECV:") + rs.err;
            return w;
        }));
    }

    // Drain accepts. 16 short connections complete inside ~200 ms on
    // a healthy box; pump a few hundred ms of reactor ticks.
    pump(60);

    int ok = 0;
    for (auto& f : clients) {
        std::string got = f.get();
        if (got == ds::proto::kWelcomeLine) ++ok;
        else ADD_FAILURE() << "session result: " << got;
    }
    EXPECT_EQ(N, ok);

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
