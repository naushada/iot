/// L16/D1 — data_store::ServiceGate tests.
///
/// Spawns ds-server as a real subprocess on a per-test mkdtemp
/// socket (matches production shape, avoids the in-process
/// reactor threading complications the in-process-server harness
/// has when callers want to do request/reply outside an async).
///
/// REQ-SVC-003 — construct primes via get(), then registers watch.
/// REQ-SVC-004 — absent key → schema-default true; thread-safe under
///               listener.
/// REQ-SVC-005 — wait() returns on change OR shutdown.
/// REQ-SVC-006 — publish_state() best-effort; no throw on failure.
/// NFR-SVC-005 — multi-waiter wakeup (every blocked thread wakes).

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
#include "data_store/proto.hpp"
#include "data_store/service_gate.hpp"
#include "data_store/value.hpp"

namespace ds = data_store;

namespace {

std::string make_temp_dir() {
    char tpl[64] = "/tmp/svcgate-XXXXXX";
    if (char* d = mkdtemp(tpl)) return d;
    ::mkdir("/tmp", 01777);
    std::strcpy(tpl, "/tmp/svcgate-XXXXXX");
    if (char* d = mkdtemp(tpl)) return d;
    std::strcpy(tpl, "./svcgate-XXXXXX");
    if (char* d = mkdtemp(tpl)) return d;
    return {};
}

/// Locate ds-server binary. CTest cwd is the build dir, so
/// `./ds-server` is the common path.
std::string find_ds_server() {
    const char* candidates[] = {
        "./ds-server",
        "../ds-server",
        "modules/data-store/build/ds-server",
        "/src/modules/data-store/build/ds-server",
    };
    for (auto p : candidates) {
        struct ::stat st;
        if (::stat(p, &st) == 0 && (st.st_mode & S_IXUSR)) return p;
    }
    return {};
}

/// Locate services.lua so the spawned ds-server can load the
/// schema (and the gate's prime sees the schema default).
std::string find_services_lua() {
    const char* candidates[] = {
        "../schemas/services.lua",
        "../../schemas/services.lua",
        "schemas/services.lua",
        "modules/data-store/schemas/services.lua",
        "/src/modules/data-store/schemas/services.lua",
    };
    for (auto p : candidates) {
        std::ifstream in(p);
        if (in.good()) return p;
    }
    return {};
}

/// Test harness: spawns ds-server as a subprocess, opens a
/// Client against its socket. Reaps the server in the dtor.
class GateFixture {
public:
    GateFixture() {
        m_dir = make_temp_dir();
        if (m_dir.empty()) return;

        m_server_bin = find_ds_server();
        if (m_server_bin.empty()) return;

        auto services_lua = find_services_lua();
        if (services_lua.empty()) return;

        // Stage a per-test schema dir containing services.lua so
        // the spawned server's `loaded N schema key(s)` line covers
        // every services.* key the gate wants to prime.
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
            // Child: exec ds-server with our per-test paths.
            std::string ds_sock_arg  = "ds-socket="     + m_sock;
            std::string ds_store_arg = "ds-store="      + m_store;
            std::string ds_sch_arg   = "ds-schema-dir=" + m_schema_dir;
            ::execlp(m_server_bin.c_str(), m_server_bin.c_str(),
                     ds_sock_arg.c_str(),
                     ds_store_arg.c_str(),
                     ds_sch_arg.c_str(),
                     nullptr);
            // exec failed.
            ::_exit(127);
        }

        // Wait up to 2s for the socket to appear.
        for (int i = 0; i < 100; ++i) {
            struct ::stat st;
            if (::stat(m_sock.c_str(), &st) == 0) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        auto cs = m_client.connect(m_sock);
        if (!cs.ok) return;
        m_ok = true;
    }

    ~GateFixture() {
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

// ─────────────────────────── REQ-SVC-003/004 ───────────────────────

TEST(SVC_REQ_SVC_003_constructor_primes_then_watches,
     absent_key_resolves_to_default_true) {
    GateFixture f;
    if (!f.ok()) GTEST_SKIP() << "ds-server binary unavailable; skipping";
    ds::ServiceGate gate(f.client(), "openvpn.client");
    EXPECT_TRUE(gate.enabled());
    EXPECT_EQ("services.openvpn.client.enable", gate.enable_key());
    EXPECT_EQ("services.openvpn.client.state",  gate.state_key());
}

TEST(SVC_REQ_SVC_004_absent_key_resolves_to_default_true,
     gate_observes_set_to_false_via_listener) {
    GateFixture f;
    if (!f.ok()) GTEST_SKIP();
    ds::ServiceGate gate(f.client(), "net.router");
    ASSERT_TRUE(gate.enabled());

    auto rs = f.client().set("services.net.router.enable", false);
    ASSERT_TRUE(rs.ok) << rs.err;

    bool observed_false = false;
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!gate.enabled()) { observed_false = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_TRUE(observed_false);
}

// ─────────────────────────── REQ-SVC-005 ───────────────────────────

TEST(SVC_REQ_SVC_005_wait_returns_on_change_or_shutdown,
     wait_returns_new_value_on_change) {
    GateFixture f;
    if (!f.ok()) GTEST_SKIP();
    ds::ServiceGate gate(f.client(), "wifi.client");
    ASSERT_TRUE(gate.enabled());

    auto waiter = std::async(std::launch::async,
                             [&] { return gate.wait(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto rs = f.client().set("services.wifi.client.enable", false);
    ASSERT_TRUE(rs.ok) << rs.err;

    auto status = waiter.wait_for(std::chrono::seconds(2));
    ASSERT_EQ(std::future_status::ready, status);
    auto got = waiter.get();
    ASSERT_TRUE(got.has_value());
    EXPECT_FALSE(*got);
}

TEST(SVC_REQ_SVC_005_wait_returns_on_change_or_shutdown,
     shutdown_returns_nullopt) {
    GateFixture f;
    if (!f.ok()) GTEST_SKIP();
    ds::ServiceGate gate(f.client(), "lwm2m.client");

    auto waiter = std::async(std::launch::async,
                             [&] { return gate.wait(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    gate.shutdown();

    auto status = waiter.wait_for(std::chrono::seconds(2));
    ASSERT_EQ(std::future_status::ready, status);
    EXPECT_FALSE(waiter.get().has_value());
}

// ─────────────────────────── NFR-SVC-005 ───────────────────────────

TEST(SVC_NFR_SVC_005_multi_waiter_wakeup,
     all_blocked_waiters_wake_on_change) {
    GateFixture f;
    if (!f.ok()) GTEST_SKIP();
    ds::ServiceGate gate(f.client(), "lwm2m.server");

    constexpr int N = 4;
    std::vector<std::future<std::optional<bool>>> futs;
    futs.reserve(N);
    for (int i = 0; i < N; ++i) {
        futs.push_back(std::async(std::launch::async,
                                  [&] { return gate.wait(); }));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto rs = f.client().set("services.lwm2m.server.enable", false);
    ASSERT_TRUE(rs.ok) << rs.err;

    int ready = 0;
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && ready < N) {
        ready = 0;
        for (auto& f2 : futs) {
            if (f2.wait_for(std::chrono::milliseconds(0))
                == std::future_status::ready) ++ready;
        }
        if (ready < N) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    EXPECT_EQ(N, ready);
    for (auto& f2 : futs) {
        auto v = f2.get();
        ASSERT_TRUE(v.has_value());
        EXPECT_FALSE(*v);
    }
}

// ─────────────────────────── REQ-SVC-006 ───────────────────────────

TEST(SVC_REQ_SVC_006_publish_state_no_throw_on_failure,
     publish_state_succeeds_and_lands) {
    GateFixture f;
    if (!f.ok()) GTEST_SKIP();
    ds::ServiceGate gate(f.client(), "openvpn.client");
    EXPECT_NO_THROW(gate.publish_state("running"));
    EXPECT_NO_THROW(gate.publish_state("disabled"));
    EXPECT_NO_THROW(gate.publish_state("stopping"));

    std::vector<ds::Client::GetResult> got;
    auto rs = f.client().get({"services.openvpn.client.state"}, got);
    ASSERT_TRUE(rs.ok);
    ASSERT_EQ(1u, got.size());
    ASSERT_TRUE(got[0].has_value);
    auto s = ds::to_string(got[0].value);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ("stopping", *s);
}
