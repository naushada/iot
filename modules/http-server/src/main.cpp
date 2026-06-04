/// iot-httpd — HTTP REST API server for the iot data store (L18/D5).
///
/// Binds a listening socket on the configured ip:port, dispatches
/// connections to HttpSession instances registered with the reactor.
/// Handlers run inline on the reactor thread (v1 — ACE_Task pool is
/// FUP for long-poll / heavy handlers).
///
/// CLI:
///   iot-httpd [ds-socket=<path>] [http-ip=<ip>] [http-port=<N>]
///            [http-scheme=http|https]
///            [http-cert=<pem>] [http-key=<pem>] [http-ca=<pem>]
///
/// With http-scheme=https the server terminates TLS itself (cert + key
/// required; a CA bundle switches on mutual-TLS). Otherwise it speaks
/// plain HTTP/1.1, as before.

#include "http_server/handler.hpp"
#include "http_server/router.hpp"
#include "http_server/session.hpp"
#include "http_server/tls.hpp"
#include "http_server/worker.hpp"

#include "data_store/client.hpp"
#include "data_store/value.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

#include <ace/INET_Addr.h>
#include <ace/Log_Msg.h>
#include <ace/Reactor.h>
#include <ace/SOCK_Acceptor.h>
#include <ace/Time_Value.h>

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int /*sig*/) {
    g_stop.store(true);
    ACE_Reactor::instance()->end_reactor_event_loop();
}

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
    // ── Config: CLI args > data-store http.* keys > defaults ──
    std::string dsPath = arg_value(argc, argv, "ds-socket");
    if (dsPath.empty()) dsPath = "/var/run/iot/data_store.sock";

    std::string httpIp = arg_value(argc, argv, "http-ip");
    if (httpIp.empty()) httpIp = "0.0.0.0";

    std::string httpPortStr = arg_value(argc, argv, "http-port");
    int httpPort = 8080;
    if (!httpPortStr.empty()) {
        httpPort = std::atoi(httpPortStr.c_str());
    }

    // TLS config (CLI > data-store http.tls.* > off). https requires
    // cert + key; a CA bundle additionally enables mutual-TLS.
    std::string httpScheme = arg_value(argc, argv, "http-scheme");
    std::string tlsCert    = arg_value(argc, argv, "http-cert");
    std::string tlsKey     = arg_value(argc, argv, "http-key");
    std::string tlsCa      = arg_value(argc, argv, "http-ca");

    // Handler thread pool size (FUP-L18-1). 0 = run handlers inline on the
    // reactor thread (default; original behaviour). >0 off-loads handlers
    // so a blocking long-poll can't stall other connections.
    std::string workersStr = arg_value(argc, argv, "http-workers");
    int httpWorkers = 0;

    // ── Connect to ds-server ──────────────────────────────────
    data_store::Client ds;
    auto cs = ds.connect(dsPath);
    if (!cs.ok) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [http:%t] %M %N:%l ds-server connect to %C "
                            "failed: %C\n"),
                   dsPath.c_str(), cs.err.c_str()));
        return 1;
    }

    // Read http.* from data store (schema defaults apply if unset).
    // Only fill a field the CLI didn't already set (CLI takes precedence).
    {
        std::vector<data_store::Client::GetResult> got;
        auto rs = ds.get({"http.listen.ip", "http.listen.port",
                          "http.listen.scheme", "http.tls.cert",
                          "http.tls.key", "http.tls.ca", "http.workers"}, got);
        if (rs.ok) {
            for (const auto& g : got) {
                if (!g.has_value) continue;
                if (g.key == "http.listen.ip") {
                    if (auto s = data_store::to_string(g.value)) httpIp = *s;
                } else if (g.key == "http.listen.port") {
                    if (auto n = data_store::to_uint32(g.value))
                        httpPort = static_cast<int>(*n);
                } else if (g.key == "http.listen.scheme" && httpScheme.empty()) {
                    if (auto s = data_store::to_string(g.value)) httpScheme = *s;
                } else if (g.key == "http.tls.cert" && tlsCert.empty()) {
                    if (auto s = data_store::to_string(g.value)) tlsCert = *s;
                } else if (g.key == "http.tls.key" && tlsKey.empty()) {
                    if (auto s = data_store::to_string(g.value)) tlsKey = *s;
                } else if (g.key == "http.tls.ca" && tlsCa.empty()) {
                    if (auto s = data_store::to_string(g.value)) tlsCa = *s;
                } else if (g.key == "http.workers" && workersStr.empty()) {
                    if (auto n = data_store::to_uint32(g.value))
                        httpWorkers = static_cast<int>(*n);
                }
            }
        }
    }
    if (httpScheme.empty()) httpScheme = "http";
    if (!workersStr.empty()) httpWorkers = std::atoi(workersStr.c_str());
    if (httpWorkers < 0) httpWorkers = 0;

    // ── TLS context (shared by every https session) ───────────
    http_server::TlsContext tlsCtx;
    const http_server::TlsContext* tlsPtr = nullptr;
    if (httpScheme == "https") {
        if (tlsCert.empty() || tlsKey.empty()) {
            ACE_ERROR((LM_ERROR,
                       ACE_TEXT("%D [http:%t] %M %N:%l https requires "
                                "http.tls.cert + http.tls.key\n")));
            return 1;
        }
        if (!tlsCtx.load_server(tlsCert, tlsKey, tlsCa)) {
            ACE_ERROR((LM_ERROR,
                       ACE_TEXT("%D [http:%t] %M %N:%l TLS init failed: %C\n"),
                       tlsCtx.err().c_str()));
            return 1;
        }
        tlsPtr = &tlsCtx;
    } else if (httpScheme != "http") {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [http:%t] %M %N:%l unknown http.listen.scheme "
                            "'%C' (expected http|https)\n"),
                   httpScheme.c_str()));
        return 1;
    }

    // ── Router + handlers ─────────────────────────────────────
    http_server::Router router;
    http_server::install_handlers(router, &ds);

    // ── Handler thread pool (FUP-L18-1) ───────────────────────
    // 0 workers → handlers run inline on the reactor thread (default).
    http_server::WorkerPool pool(static_cast<std::size_t>(httpWorkers));
    pool.start();

    // ── Acceptor ──────────────────────────────────────────────
    // Hand-rolled accept loop rather than ACE_Acceptor<HttpSession>: the
    // latter requires a default-constructible session, but HttpSession needs
    // the router (and TLS context). We open a plain ACE_SOCK_Acceptor and
    // poll it non-blocking in the event loop below, constructing each session
    // with its dependencies.
    ACE_INET_Addr addr(httpPort, httpIp.c_str());
    ACE_SOCK_Acceptor sock_acceptor;
    if (sock_acceptor.open(addr, 1) == -1) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [http:%t] %M %N:%l bind %C:%d failed "
                            "(errno=%d)\n"),
                   httpIp.c_str(), httpPort, errno));
        return 1;
    }

    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [http:%t] %M %N:%l listening on %C://%C:%d "
                        "ds=%C%C workers=%d\n"),
               httpScheme.c_str(), httpIp.c_str(), httpPort, dsPath.c_str(),
               (tlsPtr && tlsCtx.mtls()) ? " (mTLS)" : "", httpWorkers));

    // The listening socket is polled directly via non-blocking accept() in
    // the loop below; only the per-connection sessions are registered with
    // the reactor (for their READ events).
    ::signal(SIGINT,  on_signal);
    ::signal(SIGTERM, on_signal);
    ::signal(SIGPIPE, SIG_IGN);

    ACE_Time_Value tv(0, 50 * 1000);  // 50ms tick for accept
    while (!g_stop.load()) {
        // Accept pending connections
        ACE_SOCK_Stream stream;
        ACE_INET_Addr peer;
        ACE_Time_Value accept_tv(0, 0);  // non-blocking
        if (sock_acceptor.accept(stream, &peer, &accept_tv) != -1) {
            auto* session =
                new http_server::HttpSession(router, tlsPtr, &pool);
            session->set_handle(stream.get_handle());
            stream.set_handle(ACE_INVALID_HANDLE);
            if (ACE_Reactor::instance()->register_handler(
                    session, ACE_Event_Handler::READ_MASK) == -1) {
                delete session;
            }
        }

        // Run reactor event loop (one tick)
        int rc = ACE_Reactor::instance()->handle_events(tv);
        if (rc < 0 && errno != EINTR) {
            ACE_ERROR((LM_ERROR,
                       ACE_TEXT("%D [http:%t] %M %N:%l handle_events "
                                "rc=%d errno=%d\n"), rc, errno));
            break;
        }
    }

    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [http:%t] %M %N:%l shutting down\n")));
    // Join the workers before tearing down the reactor/sessions so no
    // in-flight handler is left holding a session that's about to vanish.
    pool.stop();
    sock_acceptor.close();
    return 0;
}
