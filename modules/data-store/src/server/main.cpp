/// ds-server — standalone daemon entry point for the persistent
/// key-value data store. D1 scope: bind unix socket, accept N
/// concurrent connections, send welcome line, close. Reactor-driven.
///
/// CLI:
///   ds-server [ds-socket=<path>] [ds-store=<path>]
///
/// Defaults come from data_store::proto::kDefaultSocketPath and
/// kDefaultStorePath. Both are operator-overridable.

#include "data_store.hpp"
#include "lua_persistor.hpp"
#include "schema.hpp"
#include "server.hpp"
#include "worker_pool.hpp"

#include "data_store/proto.hpp"
#include "data_store/stats_publisher.hpp"
#include "data_store/value.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include <ace/Event_Handler.h>
#include <ace/Log_Msg.h>
#include <ace/Reactor.h>
#include <ace/Time_Value.h>

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int /*sig*/) {
    g_stop.store(true);
    ACE_Reactor::instance()->end_reactor_event_loop();
}

/// L16/D2 — periodic services.ds.uptime.sec publisher. Reactor
/// timer fires every 60s and bumps the key directly via the
/// in-process DataStore::set (no socket round-trip). The handler
/// owns no resources; lifetime is tied to the main() stack.
class UptimePublisher : public ACE_Event_Handler {
public:
    UptimePublisher(data_store::server::DataStore& store,
                    std::chrono::steady_clock::time_point started)
        : m_store(store), m_started(started) {}

    int handle_timeout(const ACE_Time_Value& /*now*/,
                       const void* /*act*/) override {
        auto elapsed = std::chrono::steady_clock::now() - m_started;
        auto sec = std::chrono::duration_cast<std::chrono::seconds>(elapsed);
        std::int32_t sec_i32 = static_cast<std::int32_t>(sec.count());
        if (sec_i32 < 0) sec_i32 = 0;
        m_store.set("services.ds.uptime.sec", data_store::Value{sec_i32});
        return 0;
    }

private:
    data_store::server::DataStore&        m_store;
    std::chrono::steady_clock::time_point m_started;
};

/// Parse `key=value` args. Reused shape from apps/src/main.cpp.
std::string arg_value(int argc, char** argv, std::string_view key) {
    for (int i = 1; i < argc; ++i) {
        std::string_view a{argv[i]};
        if (a.size() > key.size() + 1 &&
            a.substr(0, key.size()) == key &&
            a[key.size()] == '=') {
            return std::string(a.substr(key.size() + 1));
        }
    }
    return {};
}

} // namespace

int main(int argc, char** argv) {
    std::string socketPath = arg_value(argc, argv, "ds-socket");
    if (socketPath.empty()) socketPath = data_store::proto::kDefaultSocketPath;

    std::string storePath = arg_value(argc, argv, "ds-store");
    if (storePath.empty())  storePath = data_store::proto::kDefaultStorePath;

    std::string schemaDir = arg_value(argc, argv, "ds-schema-dir");
    if (schemaDir.empty()) schemaDir = data_store::proto::kDefaultSchemaDir;

    std::string rateLimitStr = arg_value(argc, argv, "rate-limit-ms");
    std::uint32_t rateLimitMs = 0;
    if (!rateLimitStr.empty()) {
        rateLimitMs = static_cast<std::uint32_t>(
            std::strtoul(rateLimitStr.c_str(), nullptr, 10));
    }

    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D dsserver:thread:%t %M %N:%l socket=%C store=%C schemas=%C "
                        "rate-limit-ms=%u\n"),
               socketPath.c_str(),
               storePath.c_str(),
               schemaDir.c_str(),
               static_cast<unsigned>(rateLimitMs)));

    // Schema (optional) — load every *.lua under ds-schema-dir/. A
    // missing directory loads zero keys and the daemon still boots —
    // important on dev machines where /etc/iot/ds-schemas/ doesn't
    // exist (REQ-DS-014 keeps validation a passthrough when no schema
    // covers a key).
    auto schema = std::make_shared<data_store::server::SchemaRegistry>();
    auto n = schema->load_directory(schemaDir);
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D dsserver:thread:%t %M %N:%l loaded %u schema key(s) "
                        "from %C\n"),
               static_cast<unsigned>(n), schemaDir.c_str()));

    auto store = std::make_shared<data_store::server::DataStore>();

    // Lua-backed persistor (D4). Load the on-disk chunk into the
    // store before any worker can mutate it. Missing file → start
    // empty; corrupted file → exit 3 (REQ-DS-016).
    data_store::server::LuaPersistor persistor(storePath);
    try {
        store->load_from(persistor.load());
    } catch (const data_store::server::CorruptStoreError& e) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D dsserver:thread:%t %M %N:%l corrupt store at %C: %C — "
                            "refusing to start (exit 3)\n"),
                   storePath.c_str(), e.what()));
        return 3;
    }
    store->set_persistor(&persistor);
    store->set_rate_limit_ms(rateLimitMs);
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D dsserver:thread:%t %M %N:%l loaded %u key(s) from %C\n"),
               static_cast<unsigned>(store->size()), storePath.c_str()));

    // Active-object worker pool — N=5 threads, round-robin. Same
    // shape as xpmile MicroService pool. Must be open()ed before the
    // server starts accepting so the first handle_input has a place
    // to enqueue.
    data_store::server::WorkerPool pool(store, schema);
    if (pool.open() != 0) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D dsserver:thread:%t %M %N:%l worker pool open failed; "
                            "exit 1\n")));
        return 1;
    }

    data_store::server::Server server(store, &pool, socketPath);

    if (server.open() != 0) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D dsserver:thread:%t %M %N:%l server open failed; "
                            "exit 1\n")));
        pool.close();
        return 1;
    }

    // L16/D2 — publish services.ds.* runtime state. state is set
    // once the server is open; uptime ticks every 60s thereafter.
    // ds-server is the substrate so it can't gate ITSELF on
    // services.ds.enable (services.lua omits the enable key); the
    // state surface still exists for `ds-cli svc list` symmetry.
    const auto started = std::chrono::steady_clock::now();
    store->set("services.ds.state",
               data_store::Value{std::string("running")});
    store->set("services.ds.uptime.sec", data_store::Value{std::int32_t{0}});
    UptimePublisher uptime(*store, started);
    long uptime_timer_id = ACE_Reactor::instance()->schedule_timer(
        &uptime,
        nullptr,
        /*delay=*/ACE_Time_Value(60),
        /*interval=*/ACE_Time_Value(60));

    // L22 — resource telemetry. ds-server has no Client (it IS the store),
    // so the sink writes directly via the in-process DataStore. The main
    // thread already pumps the singleton reactor below, so the timer fires
    // there (run_reactor_thread=false).
    data_store::StatsPublisher stats(
        "services.ds",
        [&store](const std::vector<data_store::KV>& kv) {
            for (const auto& p : kv) store->set(p.first, p.second);
        });
    stats.open(data_store::StatsPublisher::STATS_FLUSH_SEC,
               /*run_reactor_thread=*/false);

    ::signal(SIGINT,  on_signal);
    ::signal(SIGTERM, on_signal);
    ::signal(SIGPIPE, SIG_IGN);

    // Reactor loop on the main thread. Wakes on SIGINT/TERM via
    // on_signal's end_reactor_event_loop call.
    ACE_Time_Value tv(1, 0);
    while (!g_stop.load()) {
        int rc = ACE_Reactor::instance()->handle_events(tv);
        if (rc < 0) {
            // -1 with errno=EINTR is normal during shutdown.
            if (errno != EINTR) {
                ACE_ERROR((LM_ERROR,
                           ACE_TEXT("%D dsserver:thread:%t %M %N:%l handle_events "
                                    "rc=%d errno=%d\n"),
                           rc, errno));
                break;
            }
        }
    }

    // Best-effort: publish exited state before tearing down.
    store->set("services.ds.state",
               data_store::Value{std::string("exited")});
    if (uptime_timer_id != -1) {
        ACE_Reactor::instance()->cancel_timer(uptime_timer_id);
    }

    server.close();
    pool.close();
    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D dsserver:thread:%t %M %N:%l clean exit\n")));
    return 0;
}
