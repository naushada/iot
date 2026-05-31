/// Per-watch callback API tests. Same Client instance, multiple
/// register calls with overlapping + distinct keys, each with its
/// own callback. The listener thread fans events out to every
/// matching callback.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include <ace/Reactor.h>
#include <ace/Time_Value.h>

#include "data_store/client.hpp"
#include "../src/server/data_store.hpp"
#include "../src/server/server.hpp"
#include "../src/server/worker_pool.hpp"

namespace ds = data_store;

namespace {

std::string make_tmpdir() {
    char tmpl[64] = "/tmp/ds-cb-XXXXXX";
    if (char* d = mkdtemp(tmpl)) return d;
    ::mkdir("/tmp", 01777);
    std::strcpy(tmpl, "/tmp/ds-cb-XXXXXX");
    if (char* d = mkdtemp(tmpl)) return d;
    std::strcpy(tmpl, "./ds-cb-XXXXXX");
    if (char* d = mkdtemp(tmpl)) return d;
    return {};
}

/// Pump the test thread's reactor for `ms` total in 20 ms slices.
void pump_ms(int ms) {
    int slices = (ms + 19) / 20;
    for (int i = 0; i < slices; ++i) {
        ACE_Time_Value tv(0, 20 * 1000);
        ACE_Reactor::instance()->handle_events(tv);
    }
}

struct LiveServer {
    std::shared_ptr<ds::server::DataStore> store;
    ds::server::WorkerPool                 pool;
    std::unique_ptr<ds::server::Server>    server;
    std::string                            sock;

    explicit LiveServer(const std::string& dir)
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

TEST(Callback, single_watch_with_callback_fires_on_event) {
    std::string dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP() << "no writable /tmp or cwd";
    LiveServer srv(dir);

    std::promise<ds::Client::Event> got;
    auto fut = got.get_future();

    auto cli_fut = std::async(std::launch::async, [&]() {
        ds::Client cli;
        auto cs = cli.connect(srv.sock); if (!cs.ok) return cs;

        ds::Client::WatchHandle h = ds::Client::kInvalidHandle;
        std::vector<std::string> keys = {"foo"};
        auto rs = cli.watch(keys,
            [&](const ds::Client::Event& e) {
                try { got.set_value(e); } catch (...) {}
            },
            &h, 2000);
        if (!rs.ok) return rs;
        EXPECT_NE(ds::Client::kInvalidHandle, h);

        // Set from the same client — server fans the notify back to
        // every watcher on the key, including us.
        auto ss = cli.set("foo", ds::Value{std::string("bar")}, 2000);
        if (!ss.ok) return ss;

        // Wait up to 3s for the callback.
        if (fut.wait_for(std::chrono::seconds(3))
                != std::future_status::ready) {
            ds::Status s; s.ok=false; s.err="callback not invoked";
            return s;
        }
        return ds::Status{};
    });

    while (cli_fut.wait_for(std::chrono::milliseconds(0))
            != std::future_status::ready) {
        pump_ms(40);
    }
    auto rs = cli_fut.get();
    EXPECT_TRUE(rs.ok) << rs.err;
    if (rs.ok) {
        auto ev = fut.get();
        EXPECT_EQ("foo", ev.key);
        EXPECT_EQ(std::string("bar"), std::get<std::string>(ev.value));
        EXPECT_FALSE(ev.prev_has_value);
    }
    ::rmdir(dir.c_str());
}

TEST(Callback, multiple_watches_with_overlapping_keys_all_fire) {
    std::string dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP() << "no writable /tmp or cwd";
    LiveServer srv(dir);

    // Per-callback counters. Callback A watches {foo, shared};
    // callback B watches {bar, shared}. A `set shared=x` MUST fire
    // both A and B; a `set foo=...` MUST fire only A.
    std::atomic<int> a_fires{0}, b_fires{0};

    auto cli_fut = std::async(std::launch::async, [&]() {
        ds::Client cli;
        auto cs = cli.connect(srv.sock); if (!cs.ok) return cs;

        ds::Client::WatchHandle ha, hb;
        std::vector<std::string> aKeys = {"foo", "shared"};
        std::vector<std::string> bKeys = {"bar", "shared"};
        auto ra = cli.watch(aKeys,
            [&](const ds::Client::Event&) { a_fires.fetch_add(1); },
            &ha, 2000);
        if (!ra.ok) return ra;
        auto rb = cli.watch(bKeys,
            [&](const ds::Client::Event&) { b_fires.fetch_add(1); },
            &hb, 2000);
        if (!rb.ok) return rb;

        if (auto s = cli.set("foo",
                ds::Value{std::string("1")}, 2000); !s.ok) return s;
        if (auto s = cli.set("bar",
                ds::Value{std::string("2")}, 2000); !s.ok) return s;
        if (auto s = cli.set("shared",
                ds::Value{std::string("3")}, 2000); !s.ok) return s;

        // Wait for listener to dispatch.
        for (int i = 0; i < 100; ++i) {
            if (a_fires.load() >= 2 && b_fires.load() >= 2) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        EXPECT_EQ(2, a_fires.load()) << "A should see foo + shared";
        EXPECT_EQ(2, b_fires.load()) << "B should see bar + shared";

        // Drop A; another set on shared should fire ONLY B.
        if (auto s = cli.unwatch(ha, 2000); !s.ok) return s;
        if (auto s = cli.set("shared",
                ds::Value{std::string("4")}, 2000); !s.ok) return s;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        EXPECT_EQ(2, a_fires.load());
        EXPECT_EQ(3, b_fires.load());
        return ds::Status{};
    });

    while (cli_fut.wait_for(std::chrono::milliseconds(0))
            != std::future_status::ready) {
        pump_ms(40);
    }
    auto rs = cli_fut.get();
    EXPECT_TRUE(rs.ok) << rs.err;
    ::rmdir(dir.c_str());
}
