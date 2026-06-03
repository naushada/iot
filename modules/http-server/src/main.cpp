/// iot-httpd — HTTP REST API server for the iot data store (L18/D5).
///
/// Binds an ACE_Acceptor on the configured ip:port, dispatches
/// connections to HttpSession instances registered with the reactor.
/// Handlers run inline on the reactor thread (v1 — ACE_Task pool is
/// FUP for long-poll / heavy handlers).
///
/// CLI:
///   iot-httpd [ds-socket=<path>] [http-ip=<ip>] [http-port=<N>]

#include "http_server/handler.hpp"
#include "http_server/router.hpp"
#include "http_server/session.hpp"

#include "data_store/client.hpp"
#include "data_store/value.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>

#include <ace/Acceptor.h>
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

using HttpAcceptor = ACE_Acceptor<http_server::HttpSession, ACE_SOCK_Acceptor>;

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

    // Read http.listen.* from data store (schema defaults apply if unset)
    {
        std::vector<data_store::Client::GetResult> got;
        auto rs = ds.get({"http.listen.ip", "http.listen.port",
                          "http.listen.scheme"}, got);
        if (rs.ok) {
            for (const auto& g : got) {
                if (!g.has_value) continue;
                if (g.key == "http.listen.ip") {
                    if (auto s = data_store::to_string(g.value)) httpIp = *s;
                } else if (g.key == "http.listen.port") {
                    if (auto n = data_store::to_uint32(g.value))
                        httpPort = static_cast<int>(*n);
                }
                // scheme is read but v1 only supports "http"
            }
        }
    }

    // ── Router + handlers ─────────────────────────────────────
    http_server::Router router;
    http_server::install_handlers(router, &ds);

    // ── Acceptor ──────────────────────────────────────────────
    ACE_INET_Addr addr(httpPort, httpIp.c_str());
    HttpAcceptor acceptor;

    // The acceptor needs a reference to the router for each new
    // session. ACE_Acceptor's default constructor pattern doesn't
    // pass extra args — we use a static/global approach.
    // v1 workaround: sessions get the router via a captured ref.
    // ACE_Acceptor<SESSION, ACE_SOCK_Acceptor> calls SESSION()
    // with no args. We wrap HttpSession in a factory acceptor.
    //
    // Alternative: use ACE_Strategy_Acceptor or hand-rolled accept.
    // v1: simple hand-rolled accept loop for clarity.

    // We'll hand-roll the accept loop to pass the router.
    ACE_SOCK_Acceptor sock_acceptor;
    if (sock_acceptor.open(addr, 1) == -1) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [http:%t] %M %N:%l bind %C:%d failed "
                            "(errno=%d)\n"),
                   httpIp.c_str(), httpPort, errno));
        return 1;
    }

    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [http:%t] %M %N:%l listening on %C:%d "
                        "ds=%C\n"),
               httpIp.c_str(), httpPort, dsPath.c_str()));

    // Register the acceptor with the reactor for READ_MASK
    if (ACE_Reactor::instance()->register_handler(
            sock_acceptor.get_handle(),
            new ACE_Event_Handler,
            ACE_Event_Handler::READ_MASK) == -1) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [http:%t] %M %N:%l register_handler failed\n")));
        return 1;
    }

    // Register a timer-based accept handler since ACE_Acceptor wiring
    // is complex with custom constructor args. Simpler: periodic timer
    // that calls accept().
    //
    // Actually, let's just do a simple event loop with accept+read+reactor.
    // The reactor handles session reads; accept is driven by the main loop.

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
            auto* session = new http_server::HttpSession(router);
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
    sock_acceptor.close();
    return 0;
}
