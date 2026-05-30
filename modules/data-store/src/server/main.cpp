/// ds-server — standalone daemon entry point for the persistent
/// key-value data store. D1 scope: bind unix socket, accept N
/// concurrent connections, send welcome line, close. Reactor-driven.
///
/// CLI:
///   ds-server [ds-socket=<path>] [ds-store=<path>]
///
/// Defaults come from data_store::proto::kDefaultSocketPath and
/// kDefaultStorePath. Both are operator-overridable.

#include "lua_persistor.hpp"
#include "server.hpp"
#include "worker_pool.hpp"

#include "data_store/proto.hpp"

#include <atomic>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include <ace/Reactor.h>
#include <ace/Time_Value.h>

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int /*sig*/) {
    g_stop.store(true);
    ACE_Reactor::instance()->end_reactor_event_loop();
}

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

    std::cout << "[ds-server] socket=" << socketPath
              << " store=" << storePath << "\n";

    auto store = std::make_shared<data_store::server::DataStore>();

    // Lua-backed persistor (D4). Load the on-disk chunk into the
    // store before any worker can mutate it. Missing file → start
    // empty; corrupted file → exit 3 (REQ-DS-016).
    data_store::server::LuaPersistor persistor(storePath);
    try {
        store->load_from(persistor.load());
    } catch (const data_store::server::CorruptStoreError& e) {
        std::cerr << "[ds-server] corrupt store at " << storePath
                  << ": " << e.what()
                  << " — refusing to start (exit 3)\n";
        return 3;
    }
    store->set_persistor(&persistor);
    std::cout << "[ds-server] loaded " << store->size()
              << " key(s) from " << storePath << "\n";

    // Active-object worker pool — N=5 threads, round-robin. Same
    // shape as xpmile MicroService pool. Must be open()ed before the
    // server starts accepting so the first handle_input has a place
    // to enqueue.
    data_store::server::WorkerPool pool(store);
    if (pool.open() != 0) {
        std::cerr << "[ds-server] worker pool open failed; exit 1\n";
        return 1;
    }

    data_store::server::Server server(store, &pool, socketPath);

    if (server.open() != 0) {
        std::cerr << "[ds-server] open failed; exit 1\n";
        pool.close();
        return 1;
    }

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
                std::cerr << "[ds-server] handle_events rc=" << rc
                          << " errno=" << errno << "\n";
                break;
            }
        }
    }

    server.close();
    pool.close();
    std::cout << "[ds-server] clean exit\n";
    return 0;
}
