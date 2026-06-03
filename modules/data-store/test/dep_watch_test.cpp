/// L17a/D1 — data_store::DepWatch tests.
///
/// Same pattern as service_gate_test.cpp: spawn a real ds-server
/// subprocess per fixture, open a Client, exercise the DepWatch.
/// Tests cover empty deps, dep state transitions, wait/wake, and
/// multi-dep unhealthy reporting.

#include <gtest/gtest.h>

#include <atomic>
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
#include "data_store/dep_watch.hpp"
#include "data_store/proto.hpp"
#include "data_store/value.hpp"

namespace ds = data_store;

namespace {

std::string make_temp_dir() {
    char tpl[64] = "/tmp/depwatch-XXXXXX";
    if (char* d = mkdtemp(tpl)) return d;
    ::mkdir("/tmp", 01777);
    std::strcpy(tpl, "/tmp/depwatch-XXXXXX");
    if (char* d = mkdtemp(tpl)) return d;
    std::strcpy(tpl, "./depwatch-XXXXXX");
    if (char* d = mkdtemp(tpl)) return d;
    return {};
}

std::string find_ds_server() {
    const char* candidates[] = {
        "./ds-server",
        "../ds-server",
        "modules/data-store/build/ds-server",
    };
    for (auto p : candidates) {
        struct ::stat st;
        if (::stat(p, &st) == 0 && (st.st_mode & S_IXUSR)) return p;
    }
    return {};
}

std::string find_services_lua() {
    const char* candidates[] = {
        "../schemas/services.lua",
        "../../schemas/services.lua",
        "schemas/services.lua",
        "modules/data-store/schemas/services.lua",
    };
    for (auto p : candidates) {
        std::ifstream in(p);
        if (in.good()) return p;
    }
    return {};
}

class DepWatchFixture {
public:
    DepWatchFixture() {
        m_dir = make_temp_dir();
        if (m_dir.empty()) return;

        m_server_bin = find_ds_server();
        if (m_server_bin.empty()) return;

        auto services_lua = find_services_lua();
        if (services_lua.empty()) return;

        m_schema_dir = m_dir + "/schemas";
        ::mkdir(m_schema_dir.c_str(), 0755);
        std::ifstream in(services_lua);
        std::ofstream out(m_schema_dir + "/services.lua");
        out << in.rdbuf();

        m_sock  = m_dir + "/ds.sock";
        m_store = m_dir + "/store.lua";

        m_server_pid = ::fork();
        if (m_server_pid < 0) return;
        if (m_server_pid == 0) {
            std::string ds_sock_arg  = "ds-socket="     + m_sock;
            std::string ds_store_arg = "ds-store="      + m_store;
            std::string ds_sch_arg   = "ds-schema-dir=" + m_schema_dir;
            ::execlp(m_server_bin.c_str(), m_server_bin.c_str(),
                     ds_sock_arg.c_str(),
                     ds_store_arg.c_str(),
                     ds_sch_arg.c_str(),
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
        m_ok = true;
    }

    ~DepWatchFixture() {
        m_client.close();
        if (m_server_pid > 0) {
            ::kill(m_server_pid, SIGTERM);
            int status = 0;
            for (int i = 0; i < 20; ++i) {
                pid_t r = ::waitpid(m_server_pid, &status, WNOHANG);
                if (r == m_server_pid) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            ::kill(m_server_pid, SIGKILL);
            ::waitpid(m_server_pid, &status, 0);
        }
    }

    bool ok() const { return m_ok; }
    ds::Client& client() { return m_client; }

private:
    std::string  m_dir;
    std::string  m_server_bin;
    std::string  m_schema_dir;
    std::string  m_sock;
    std::string  m_store;
    pid_t        m_server_pid = -1;
    ds::Client   m_client;
    bool         m_ok = false;
};

} // namespace

// ─────────── DEP_REQ_001: empty deps always healthy ────────────

TEST(DEP_REQ_001_empty_deps_always_healthy,
     no_deps_healthy_returns_true) {
    DepWatchFixture f;
    if (!f.ok()) GTEST_SKIP() << "ds-server binary unavailable; skipping";
    ds::DepWatch dw(f.client(), {});
    EXPECT_TRUE(dw.healthy());
    EXPECT_EQ("", dw.unhealthy_dep());
    EXPECT_EQ(0u, dw.count());
    // wait() on empty deps never wakes on its own; must be shutdown().
    auto waiter = std::async(std::launch::async,
                             [&] { return dw.wait(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    dw.shutdown();
    auto status = waiter.wait_for(std::chrono::seconds(2));
    ASSERT_EQ(std::future_status::ready, status);
    EXPECT_FALSE(waiter.get());   // shutdown → false
}

// ─────────── DEP_REQ_002: dep disabled makes unhealthy ─────────

TEST(DEP_REQ_002_dep_disabled_makes_unhealthy,
     set_dep_state_disabled_unhealthy_dep_returns_name) {
    DepWatchFixture f;
    if (!f.ok()) GTEST_SKIP();
    ds::DepWatch dw(f.client(), {"net.router"});
    ASSERT_TRUE(dw.healthy());

    // Set net.router state to "disabled".
    auto rs = f.client().set("services.net.router.state",
                             std::string("disabled"));
    ASSERT_TRUE(rs.ok) << rs.err;

    bool observed_unhealthy = false;
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!dw.healthy()) { observed_unhealthy = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(observed_unhealthy);
    EXPECT_EQ("net.router", dw.unhealthy_dep());
}

// ─────────── DEP_REQ_003: starting counts as healthy ───────────

TEST(DEP_REQ_003_dep_starting_counts_as_healthy,
     starting_state_is_healthy) {
    DepWatchFixture f;
    if (!f.ok()) GTEST_SKIP();
    ds::DepWatch dw(f.client(), {"net.router"});
    ASSERT_TRUE(dw.healthy());

    // Set dep to "starting" — transient but healthy.
    auto rs = f.client().set("services.net.router.state",
                             std::string("starting"));
    ASSERT_TRUE(rs.ok) << rs.err;

    // The watch fires on change; give it a moment.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    EXPECT_TRUE(dw.healthy());
    EXPECT_EQ("", dw.unhealthy_dep());
}

// ─────────── DEP_REQ_004: wait returns on state change ─────────

TEST(DEP_REQ_004_wait_returns_on_state_change,
     wait_wakes_on_dep_state_transition) {
    DepWatchFixture f;
    if (!f.ok()) GTEST_SKIP();
    ds::DepWatch dw(f.client(), {"net.router"});
    ASSERT_TRUE(dw.healthy());

    auto waiter = std::async(std::launch::async,
                             [&] { return dw.wait(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto rs = f.client().set("services.net.router.state",
                             std::string("disabled"));
    ASSERT_TRUE(rs.ok) << rs.err;

    auto status = waiter.wait_for(std::chrono::seconds(2));
    ASSERT_EQ(std::future_status::ready, status);
    EXPECT_TRUE(waiter.get());   // state change → true
    EXPECT_FALSE(dw.healthy());
}

// ─────────── DEP_REQ_005: multi-dep, first unhealthy wins ──────

TEST(DEP_REQ_005_multi_dep_first_unhealthy_wins,
     two_deps_second_disabled_reports_second) {
    // When multiple dependencies are watched and one goes unhealthy,
    // unhealthy_dep() returns the first one it encounters — but
    // since the order matches the input vector, the first
    // chronologically-unhealthy is reported.
    DepWatchFixture f;
    if (!f.ok()) GTEST_SKIP();
    ds::DepWatch dw(f.client(), {"openvpn.client", "net.router"});
    ASSERT_TRUE(dw.healthy());
    EXPECT_EQ(2u, dw.count());

    // Disable net.router (the second dep).
    auto rs = f.client().set("services.net.router.state",
                             std::string("disabled"));
    ASSERT_TRUE(rs.ok) << rs.err;

    bool observed_unhealthy = false;
    std::string bad_dep;
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!dw.healthy()) {
            observed_unhealthy = true;
            bad_dep = dw.unhealthy_dep();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(observed_unhealthy);
    EXPECT_EQ("net.router", bad_dep);

    // Recover net.router.
    rs = f.client().set("services.net.router.state",
                        std::string("running"));
    ASSERT_TRUE(rs.ok) << rs.err;

    auto deadline2 = std::chrono::steady_clock::now()
                   + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline2) {
        if (dw.healthy()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(dw.healthy());
}
