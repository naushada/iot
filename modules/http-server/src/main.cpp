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

#include "auth.hpp"
#include "handler.hpp"
#include "router.hpp"
#include "session.hpp"
#include "tls.hpp"
#include "worker.hpp"

#include "data_store/client.hpp"
#include "data_store/value.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>

#include <ace/INET_Addr.h>
#include <ace/Log_Msg.h>
#include <ace/Log_Record.h>
#include <ace/Reactor.h>
#include <ace/SOCK_Acceptor.h>
#include <ace/Time_Value.h>

namespace {

std::atomic<bool> g_stop{false};

// ── Log ring buffer ────────────────────────────────────────────────
// Captures ACE log output into a fixed-size deque so the UI can tail
// logs via GET /api/v1/log → log.text in the data store.

#include <deque>
#include <mutex>

std::mutex g_log_mutex;
std::deque<std::string> g_log_buf;
constexpr std::size_t kMaxLogLines = 200;

// ACE_Log_Msg callback — intercepts every ACE_DEBUG / ACE_ERROR etc.
class LogCallback : public ACE_Log_Msg_Callback {
public:
    void log(ACE_Log_Record& rec) override {
        std::time_t t = static_cast<std::time_t>(rec.time_stamp().sec());
        struct std::tm tm_buf;
        ::localtime_r(&t, &tm_buf);
        char ts[16];
        std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm_buf);

        const char* lvl = "?";
        switch (rec.type()) {
            case LM_DEBUG:   lvl = "DEBUG"; break;
            case LM_INFO:    lvl = "INFO";  break;
            case LM_NOTICE:  lvl = "NOTE";  break;
            case LM_WARNING: lvl = "WARN";  break;
            case LM_ERROR:   lvl = "ERROR"; break;
            case LM_CRITICAL:lvl = "CRIT";  break;
        }

        std::lock_guard<std::mutex> lk(g_log_mutex);
        g_log_buf.push_back(
            std::string(ts) + " " + lvl + " httpd: " +
            rec.msg_data() + "\n");
        while (g_log_buf.size() > kMaxLogLines) g_log_buf.pop_front();
    }
};

void flush_log_to_ds(data_store::Client& ds) {
    std::string text;
    {
        std::lock_guard<std::mutex> lk(g_log_mutex);
        for (const auto& line : g_log_buf) text += line;
    }
    ds.set("log.text", data_store::Value{text}, 200);  // best-effort
}

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

// Raw snapshot of the hot-reloadable http.* keys (empty string = unset).
// Used to detect operator changes made via ds-cli at runtime (FUP-L18-2).
struct DsHttpCfg {
    std::string ip, port, scheme, cert, key, ca;
};

DsHttpCfg read_ds_http_cfg(data_store::Client& ds) {
    DsHttpCfg c;
    std::vector<data_store::Client::GetResult> got;
    auto rs = ds.get({"http.listen.ip", "http.listen.port", "http.listen.scheme",
                      "http.tls.cert", "http.tls.key", "http.tls.ca"}, got);
    if (rs.ok) {
        for (const auto& g : got) {
            if (!g.has_value) continue;
            if (g.key == "http.listen.ip") {
                if (auto s = data_store::to_string(g.value)) c.ip = *s;
            } else if (g.key == "http.listen.port") {
                if (auto n = data_store::to_uint32(g.value))
                    c.port = std::to_string(*n);
            } else if (g.key == "http.listen.scheme") {
                if (auto s = data_store::to_string(g.value)) c.scheme = *s;
            } else if (g.key == "http.tls.cert") {
                if (auto s = data_store::to_string(g.value)) c.cert = *s;
            } else if (g.key == "http.tls.key") {
                if (auto s = data_store::to_string(g.value)) c.key = *s;
            } else if (g.key == "http.tls.ca") {
                if (auto s = data_store::to_string(g.value)) c.ca = *s;
            }
        }
    }
    return c;
}

} // namespace

int main(int argc, char** argv) {
    // ── Config: CLI args > data-store http.* keys > defaults ──
    std::string dsPath = arg_value(argc, argv, "ds-socket");
    if (dsPath.empty()) dsPath = "/var/run/iot/data_store.sock";

    std::string httpIpCli = arg_value(argc, argv, "http-ip");
    std::string httpIp = httpIpCli.empty() ? "0.0.0.0" : httpIpCli;

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
                if (g.key == "http.listen.ip" && httpIpCli.empty()) {
                    if (auto s = data_store::to_string(g.value)) httpIp = *s;
                } else if (g.key == "http.listen.port" && httpPortStr.empty()) {
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

    // ── Auth ──────────────────────────────────────────────────
    http_server::SessionStore auth_store;

    // Load auth enable flag + admin password from data store.
    {
        std::vector<data_store::Client::GetResult> got;
        auto rs = ds.get({"http.auth.enabled"}, got);
        if (rs.ok && !got.empty() && got[0].has_value) {
            if (auto b = data_store::to_bool(got[0].value))
                auth_store.set_enabled(*b);
        }
    }

    // ── Static file serving (SPA) ─────────────────────────────
    std::string wwwDir = arg_value(argc, argv, "www-dir");
    if (wwwDir.empty()) {
        // Default: look for iot-ui/dist/iot-ui relative to the
        // http-server binary, or next to the repo root.
        wwwDir = "/usr/share/iot/www";
    }

    // ── Router + handlers ─────────────────────────────────────
    http_server::Router router;
    if (!wwwDir.empty()) router.set_static_dir(wwwDir);
    http_server::install_handlers(router, &ds, &auth_store);

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

    // Intercept ACE log output → ring buffer → flushed to ds log.text
    static LogCallback log_cb;
    ACE_Log_Msg::instance()->msg_callback(&log_cb);

    // ── Hot-reload (FUP-L18-2) ────────────────────────────────
    // Poll the hot-reloadable http.* keys; when an operator changes one via
    // ds-cli at runtime, apply it without a restart. ds values are
    // authoritative at runtime (CLI args are startup defaults). http.workers
    // is NOT hot-reloadable — resizing the pool needs a restart.
    DsHttpCfg lastDs = read_ds_http_cfg(ds);

    // Rebuild the TLS context from the current effective scheme/cert/key/ca.
    // On https, SSL_new() in live TlsConns holds a ref to the old SSL_CTX, so
    // swapping tlsCtx is safe: existing connections keep their cert until they
    // close; new ones use the rotated cert. A failed load keeps the old.
    auto reload_tls = [&]() {
        if (httpScheme == "https") {
            if (tlsCert.empty() || tlsKey.empty()) {
                ACE_ERROR((LM_ERROR,
                           ACE_TEXT("%D [http:%t] %M %N:%l reload: https needs "
                                    "cert+key — keeping current\n")));
                return;
            }
            http_server::TlsContext fresh;
            if (!fresh.load_server(tlsCert, tlsKey, tlsCa)) {
                ACE_ERROR((LM_ERROR,
                           ACE_TEXT("%D [http:%t] %M %N:%l reload: TLS load "
                                    "failed: %C — keeping current\n"),
                           fresh.err().c_str()));
                return;
            }
            tlsCtx = std::move(fresh);
            tlsPtr = &tlsCtx;
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D [http:%t] %M %N:%l TLS reloaded "
                                "(mtls=%d)\n"), tlsCtx.mtls()));
        } else {
            tlsPtr = nullptr;
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D [http:%t] %M %N:%l scheme=http — TLS off "
                                "for new connections\n")));
        }
    };

    // Re-bind the listening socket to a new ip:port. Open the new one first
    // and only swap on success, so a failed bind (e.g. port in use) leaves
    // the current listener serving.
    auto rebind = [&]() {
        ACE_INET_Addr na(httpPort, httpIp.c_str());
        ACE_SOCK_Acceptor fresh;
        if (fresh.open(na, 1) == -1) {
            ACE_ERROR((LM_ERROR,
                       ACE_TEXT("%D [http:%t] %M %N:%l reload: bind %C:%d "
                                "failed (errno=%d) — keeping current\n"),
                       httpIp.c_str(), httpPort, errno));
            return;
        }
        sock_acceptor.close();
        sock_acceptor.set_handle(fresh.get_handle());
        fresh.set_handle(ACE_INVALID_HANDLE);
        ACE_DEBUG((LM_INFO,
                   ACE_TEXT("%D [http:%t] %M %N:%l re-bound to %C:%d\n"),
                   httpIp.c_str(), httpPort));
    };

    ACE_Time_Value tv(0, 50 * 1000);  // 50ms tick for accept
    int reload_tick = 0;
    constexpr int kReloadEvery = 40;  // 40 * 50ms ≈ 2s
    while (!g_stop.load()) {
        // Accept pending connections
        ACE_SOCK_Stream stream;
        ACE_INET_Addr peer;
        ACE_Time_Value accept_tv(0, 0);  // non-blocking
        if (sock_acceptor.accept(stream, &peer, &accept_tv) != -1) {
            auto* session =
                new http_server::HttpSession(router, tlsPtr, &pool,
                                             &auth_store);
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

        // ~Every 2s, check for operator config changes via the data store.
        // Also sweep expired sessions and reload auth config every ~60s.
        if (++reload_tick >= kReloadEvery) {
            reload_tick = 0;
            static int auth_tick = 0;
            if (++auth_tick >= 30) {  // ~60s
                auth_tick = 0;
                auth_store.sweep_expired();
                // Reload auth.enabled
                std::vector<data_store::Client::GetResult> got;
                auto rs = ds.get({"http.auth.enabled"}, got);
                if (rs.ok && !got.empty() && got[0].has_value) {
                    if (auto b = data_store::to_bool(got[0].value))
                        auth_store.set_enabled(*b);
                }
                // Password hash is reloaded on each login attempt
                // (stateless — no cache to invalidate).

                // Reload log level
                {
                    std::vector<data_store::Client::GetResult> lg;
                    auto ls = ds.get({"log.level"}, lg);
                    if (ls.ok && !lg.empty() && lg[0].has_value) {
                        if (auto lv = data_store::to_string(lg[0].value)) {
                            unsigned long mask = LM_INFO;  // default
                            std::string upper = *lv;
                            for (auto& c : upper) c = static_cast<char>(std::toupper(c));
                            if (upper == "DEBUG")    mask = LM_DEBUG;
                            else if (upper == "INFO")    mask = LM_INFO;
                            else if (upper == "WARNING") mask = LM_WARNING;
                            else if (upper == "ERROR")   mask = LM_ERROR | LM_CRITICAL;
                            ACE_Log_Msg::instance()->priority_mask(
                                static_cast<int>(mask), ACE_Log_Msg::PROCESS);
                        }
                    }
                }
                // Flush ring-buffer logs to data store so the UI can tail them
                flush_log_to_ds(ds);
            }
            DsHttpCfg cur = read_ds_http_cfg(ds);
            bool tlsDirty = false, listenDirty = false;
            // Update the effective values for keys the operator changed.
            // A non-empty scheme/ip/port is required to take effect; tls
            // paths may change freely (reload_tls validates).
            if (cur.scheme != lastDs.scheme && !cur.scheme.empty()) {
                httpScheme = cur.scheme; tlsDirty = true;
            }
            if (cur.cert != lastDs.cert) { tlsCert = cur.cert; tlsDirty = true; }
            if (cur.key  != lastDs.key)  { tlsKey  = cur.key;  tlsDirty = true; }
            if (cur.ca   != lastDs.ca)   { tlsCa   = cur.ca;   tlsDirty = true; }
            if (cur.ip != lastDs.ip && !cur.ip.empty()) {
                httpIp = cur.ip; listenDirty = true;
            }
            if (cur.port != lastDs.port && !cur.port.empty()) {
                httpPort = std::atoi(cur.port.c_str()); listenDirty = true;
            }
            lastDs = cur;
            if (tlsDirty) reload_tls();
            if (listenDirty) rebind();
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
