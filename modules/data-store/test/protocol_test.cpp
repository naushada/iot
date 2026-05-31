/// End-to-end protocol tests: spin a real Server+WorkerPool, drive
/// it with one or two real Clients, exercise set/get/register/remove
/// and the changed-event push.

#include <gtest/gtest.h>

#include <chrono>
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
#include "../src/server/data_store.hpp"
#include "../src/server/server.hpp"
#include "../src/server/worker_pool.hpp"

namespace ds = data_store;

namespace {

std::string make_temp_dir() {
    char tmpl[64] = "/tmp/ds-proto-XXXXXX";
    if (char* d = mkdtemp(tmpl)) return d;
    ::mkdir("/tmp", 01777);
    std::strcpy(tmpl, "/tmp/ds-proto-XXXXXX");
    if (char* d = mkdtemp(tmpl)) return d;
    std::strcpy(tmpl, "./ds-proto-XXXXXX");
    if (char* d = mkdtemp(tmpl)) return d;
    return {};
}

void pump(int times = 30) {
    ACE_Time_Value tv(0, 50 * 1000);     // 50 ms each
    for (int i = 0; i < times; ++i) {
        ACE_Reactor::instance()->handle_events(tv);
    }
}

/// Pump the reactor in 20 ms slices until every future is ready (or
/// a hard 8 s deadline elapses). Needed because async client tasks
/// block on socket I/O that the test thread's reactor must service —
/// a fixed pump() count starves the futures when their wall-clock
/// work exceeds it.
template <typename... Fs>
void pump_until(Fs&... fs) {
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + seconds(8);
    while (steady_clock::now() < deadline) {
        const bool all_ready =
            ((fs.wait_for(milliseconds{0}) == std::future_status::ready) && ...);
        if (all_ready) return;
        ACE_Time_Value tv(0, 20 * 1000);
        ACE_Reactor::instance()->handle_events(tv);
    }
}

struct LiveServer {
    std::shared_ptr<ds::server::DataStore> store;
    ds::server::WorkerPool                 pool;
    std::unique_ptr<ds::server::Server>    server;
    std::string                            sock;

    LiveServer(const std::string& dir)
      : store(std::make_shared<ds::server::DataStore>()),
        pool(store, /*schema=*/nullptr, 3),
        sock(dir + "/ds.sock") {
        EXPECT_EQ(0, pool.open());
        server = std::make_unique<ds::server::Server>(store, &pool, sock);
        EXPECT_EQ(0, server->open());
    }

    ~LiveServer() {
        server->close();
        pool.close();
        ::unlink(sock.c_str());
    }
};

} // namespace

/* ─────────────────────────── REQ-DS-003 ──────────────────────────── */

TEST(Protocol, DS_REQ_DS_003_set_then_get_returns_latest) {
    std::string dir = make_temp_dir();
    if (dir.empty()) GTEST_SKIP() << "no writable /tmp or cwd";
    LiveServer srv(dir);

    auto cli_fut = std::async(std::launch::async, [&]() {
        ds::Client cli;
        auto cs = cli.connect(srv.sock); if (!cs.ok) return cs;
        std::string w; cli.recv_welcome(w, 2000);
        auto ss = cli.set("foo", ds::Value{std::string("bar")}, 2000);
        if (!ss.ok) return ss;
        std::vector<ds::Client::GetResult> got;
        auto gs = cli.get({"foo", "missing"}, got, 2000); if (!gs.ok) return gs;
        EXPECT_EQ(2u, got.size());
        EXPECT_EQ("foo", got[0].key);
        EXPECT_TRUE(got[0].has_value);
        EXPECT_EQ(std::string("bar"), std::get<std::string>(got[0].value));
        EXPECT_EQ("missing", got[1].key);
        EXPECT_FALSE(got[1].has_value);
        return ds::Status{};
    });

    pump_until(cli_fut);
    auto s = cli_fut.get();
    EXPECT_TRUE(s.ok) << s.err;
    ::rmdir(dir.c_str());
}

/* ─────────────────────── REQ-DS-004 + 007 ────────────────────────── */

TEST(Protocol, DS_REQ_DS_007_register_then_set_emits_notify_to_watcher) {
    std::string dir = make_temp_dir();
    if (dir.empty()) GTEST_SKIP() << "no writable /tmp or cwd";
    LiveServer srv(dir);

    // Watcher client: connect, register "foo", then read one event.
    auto watch_fut = std::async(std::launch::async, [&]() {
        ds::Client w;
        auto cs = w.connect(srv.sock); if (!cs.ok) return cs;
        std::string welcome; w.recv_welcome(welcome, 2000);
        auto rs = w.watch("foo", 2000); if (!rs.ok) return rs;

        ds::Client::Event ev;
        auto es = w.recv_event(ev, 5000);
        if (!es.ok) return es;
        EXPECT_EQ("foo",  ev.key);
        EXPECT_EQ(std::string("bar"), std::get<std::string>(ev.value));
        EXPECT_FALSE(ev.prev_has_value);   // foo was new
        return ds::Status{};
    });

    // Setter client: connect, give the watcher a head start, then
    // set foo=bar.
    auto set_fut = std::async(std::launch::async, [&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ds::Client s;
        auto cs = s.connect(srv.sock); if (!cs.ok) return cs;
        std::string welcome; s.recv_welcome(welcome, 2000);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return s.set("foo", ds::Value{std::string("bar")}, 2000);
    });

    pump_until(watch_fut, set_fut);
    auto ws = watch_fut.get();
    auto ss = set_fut.get();
    EXPECT_TRUE(ws.ok) << "watcher: " << ws.err;
    EXPECT_TRUE(ss.ok) << "setter: " << ss.err;
    ::rmdir(dir.c_str());
}

/* ─────────────────────────── REQ-DS-006 ──────────────────────────── */

TEST(Protocol, DS_REQ_DS_006_unchanged_value_no_notify) {
    std::string dir = make_temp_dir();
    if (dir.empty()) GTEST_SKIP() << "no writable /tmp or cwd";
    LiveServer srv(dir);

    // Pre-set foo=bar via a throwaway client, then have a watcher
    // register and a setter rewrite the same value — watcher should
    // see no event within 300 ms.
    auto seed_fut = std::async(std::launch::async, [&]() {
        ds::Client s;
        auto cs = s.connect(srv.sock); if (!cs.ok) return cs;
        std::string w; s.recv_welcome(w, 2000);
        return s.set("foo", ds::Value{std::string("bar")}, 2000);
    });
    pump_until(seed_fut);
    auto seed = seed_fut.get();
    ASSERT_TRUE(seed.ok) << seed.err;

    auto watch_fut = std::async(std::launch::async, [&]() {
        ds::Client w;
        auto cs = w.connect(srv.sock); if (!cs.ok) return cs;
        std::string welcome; w.recv_welcome(welcome, 2000);
        auto rs = w.watch("foo", 2000); if (!rs.ok) return rs;
        ds::Client::Event ev;
        auto es = w.recv_event(ev, 300);
        // ETIMEDOUT is the success case here.
        if (es.code == ETIMEDOUT) return ds::Status{};
        ds::Status fail;
        fail.ok = false;
        fail.err = es.ok
            ? "unexpected notify k=" + ev.key
            : "unexpected error: " + es.err;
        return fail;
    });

    auto set_fut = std::async(std::launch::async, [&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ds::Client s;
        auto cs = s.connect(srv.sock); if (!cs.ok) return cs;
        std::string welcome; s.recv_welcome(welcome, 2000);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return s.set("foo", ds::Value{std::string("bar")}, 2000);   // same value
    });

    pump_until(watch_fut, set_fut);
    auto ws = watch_fut.get();
    auto ss = set_fut.get();
    EXPECT_TRUE(ws.ok) << "watcher: " << ws.err;
    EXPECT_TRUE(ss.ok) << "setter: " << ss.err;
    ::rmdir(dir.c_str());
}
