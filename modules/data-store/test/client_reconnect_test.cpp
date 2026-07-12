/// Client auto-reconnect across a ds-server restart.
///
/// Spawns ds-server as a real subprocess (same shape as
/// service_gate_test), then kills and respawns it on the SAME socket
/// path — the production scenario of `systemctl restart iot-ds`.
///
/// Before the fix, every long-lived daemon went permanently deaf after
/// a ds restart: set() failed forever (dead socket, `connected` never
/// cleared) and watches were never re-registered on the new server
/// instance (cellular-client stopped publishing, iot-containerd
/// stopped seeing container.cmd.request).
///
/// REQ-DS-CLI-RECONNECT-001 — set() recovers after a server restart.
/// REQ-DS-CLI-RECONNECT-002 — callback watches fire again after a
///                            server restart (auto re-register).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "data_store/client.hpp"

namespace ds = data_store;

namespace {

std::string make_temp_dir() {
    char tmpl[] = "/tmp/ds-reconn-XXXXXX";
    char* p = ::mkdtemp(tmpl);
    return p ? std::string(p) : std::string();
}

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

/// Restartable subprocess ds-server on a fixed socket path.
class ReconnectFixture {
public:
    ReconnectFixture() {
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

        if (!spawn_server()) return;

        auto cs = m_client.connect(m_sock);
        if (!cs.ok) return;
        m_ok = true;
    }

    ~ReconnectFixture() {
        m_client.close();
        kill_server();
    }

    bool spawn_server() {
        // The old socket file lingers after a kill; the server unlinks
        // and rebinds it on startup, exactly like the systemd restart.
        m_server_pid = ::fork();
        if (m_server_pid < 0) return false;
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
        // Wait for the (fresh) socket to accept: poll until a probe
        // client can connect, up to 2s.
        for (int i = 0; i < 100; ++i) {
            ds::Client probe;
            if (probe.connect(m_sock).ok) { probe.close(); return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }

    void kill_server() {
        if (m_server_pid <= 0) return;
        ::kill(m_server_pid, SIGKILL);
        int status = 0;
        ::waitpid(m_server_pid, &status, 0);
        m_server_pid = -1;
    }

    /// The client reconnects in the background (500ms retry cadence);
    /// poll set() until it sticks or the deadline passes.
    bool wait_set_ok(const std::string& key, const std::string& val,
                     int deadline_ms = 5000) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(deadline_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (m_client.set(key, ds::Value{val}).ok) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }

    bool ok() const { return m_ok; }
    ds::Client& client() { return m_client; }
    const std::string& sock() const { return m_sock; }

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

constexpr const char* kKey = "services.wifi.client.state";

} // namespace

TEST(ClientReconnect, set_recovers_after_server_restart) {
    ReconnectFixture fx;
    if (!fx.ok()) GTEST_SKIP() << "ds-server binary/schema not found";

    ASSERT_TRUE(fx.client().set(kKey, ds::Value{std::string("running")}).ok);

    fx.kill_server();
    // The very next set on the dead socket must fail (no server), not
    // hang — and must not poison the client.
    EXPECT_FALSE(fx.client().set(kKey, ds::Value{std::string("x")}).ok);

    ASSERT_TRUE(fx.spawn_server());
    EXPECT_TRUE(fx.wait_set_ok(kKey, "stopped"))
        << "set() never recovered after the server came back";
}

TEST(ClientReconnect, watch_refires_after_server_restart) {
    ReconnectFixture fx;
    if (!fx.ok()) GTEST_SKIP() << "ds-server binary/schema not found";

    std::atomic<int> events{0};
    ds::Client::WatchHandle wh = ds::Client::kInvalidHandle;
    ASSERT_TRUE(fx.client().watch(
        std::vector<std::string>{kKey},
        [&events](const ds::Client::Event&) { ++events; },
        &wh).ok);

    // Writer client drives changes; the watcher must see them.
    ds::Client writer;
    ASSERT_TRUE(writer.connect(fx.sock()).ok);
    ASSERT_TRUE(writer.set(kKey, ds::Value{std::string("running")}).ok);
    for (int i = 0; i < 50 && events.load() < 1; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    ASSERT_GE(events.load(), 1) << "watch never fired pre-restart";

    fx.kill_server();
    writer.close();
    ASSERT_TRUE(fx.spawn_server());

    // Wait until the watcher client has re-attached (its own set works
    // again), then change the key from a fresh writer.
    ASSERT_TRUE(fx.wait_set_ok(kKey, "stopped"));
    const int before = events.load();

    ds::Client writer2;
    ASSERT_TRUE(writer2.connect(fx.sock()).ok);
    ASSERT_TRUE(writer2.set(kKey, ds::Value{std::string("running")}).ok);
    for (int i = 0; i < 100 && events.load() <= before; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    writer2.close();

    EXPECT_GT(events.load(), before)
        << "watch was not re-registered on the new server instance";
}

/// REQ-DS-CLI-RECONNECT-003 — a watch whose RegisterWatch could not reach the
/// server must still be honoured once the client re-attaches.
///
/// The failure this pins down was found on hardware: cellular-client registers
/// three watches at startup, one RegisterWatch did not make it, and the client
/// ERASED that key from its refcount. try_reconnect() re-registers from exactly
/// that refcount, so the key could never come back — the daemon logged
/// "re-watching 2 key(s)" for the rest of its life and silently ignored
/// sms.clear.request forever. Callers do not check watch()'s Status, so nothing
/// surfaced. A failed registration must leave the subscription intact.
TEST(ClientReconnect, watch_registered_while_server_down_heals_on_reconnect) {
    ReconnectFixture fx;
    if (!fx.ok()) GTEST_SKIP() << "ds-server binary/schema not found";

    fx.kill_server();

    // Register the watch with NO server listening: the wire RegisterWatch cannot
    // land, so this reports an error — and the subscription must survive it.
    std::atomic<int> events{0};
    ds::Client::WatchHandle wh = ds::Client::kInvalidHandle;
    const auto ws = fx.client().watch(
        std::vector<std::string>{kKey},
        [&events](const ds::Client::Event&) { ++events; },
        &wh);
    EXPECT_FALSE(ws.ok) << "watch() on a dead socket must report the failure";

    ASSERT_TRUE(fx.spawn_server());
    ASSERT_TRUE(fx.wait_set_ok(kKey, "stopped"))
        << "client never re-attached to the new server";

    ds::Client writer;
    ASSERT_TRUE(writer.connect(fx.sock()).ok);
    ASSERT_TRUE(writer.set(kKey, ds::Value{std::string("running")}).ok);
    for (int i = 0; i < 100 && events.load() < 1; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    writer.close();

    EXPECT_GE(events.load(), 1)
        << "a watch that failed to register was dropped for good instead of "
           "being re-registered on reconnect";
}
