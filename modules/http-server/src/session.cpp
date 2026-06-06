#include "auth.hpp"
#include "session.hpp"

#include <ace/Log_Msg.h>
#include <ace/Reactor.h>
#include <ace/Time_Value.h>

#include <cstring>
#include <memory>
#include <utility>

namespace http_server {

HttpSession::HttpSession(const Router& router, const TlsContext* tls,
                         WorkerPool* pool, SessionStore* auth)
    : m_router(router), m_pool(pool), m_auth(auth) {
    if (tls && *tls) {
        m_tls = std::make_unique<TlsConn>(*tls);
    }
    // No parser handler: the session drives routing itself (inline or, with
    // a worker pool, off the reactor thread). The parser just parses.
}

int HttpSession::handle_input(ACE_HANDLE /*fd*/) {
    // Idle timeout for keep-alive connections: close if no request arrives
    // within kIdleTimeoutSec of the last response.  First-request connections
    // that haven't completed a response yet get the full timeout.
    auto now = std::chrono::steady_clock::now();
    if (m_keep_alive &&
        now - m_last_active > std::chrono::seconds(kIdleTimeoutSec)) {
        return -1;  // close idle keep-alive connection
    }

    char buf[4096];
    ACE_Time_Value tv(0, 100 * 1000);  // 100ms timeout for keep-alive
    ssize_t n = peer().recv(buf, sizeof(buf), &tv);

    if (n <= 0) {
        if (n == 0 || (errno != ETIME && errno != ETIMEDOUT && errno != EAGAIN)) {
            return -1;  // close
        }
        return 0;  // timeout — stay registered
    }

    // Turn this read into plaintext application bytes. On https the bytes
    // off the wire are ciphertext: feed them to the TLS engine, advance the
    // handshake, then decrypt. On plain HTTP the bytes are the request.
    std::string app;
    if (m_tls) {
        m_tls->feed_ciphertext(buf, static_cast<std::size_t>(n));

        if (!m_tls->handshake_done()) {
            int hs = m_tls->handshake();
            // Flush handshake output (ServerHello, certificate, …) to the peer.
            std::string out;
            if (m_tls->drain_outgoing(out)) peer().send_n(out.data(), out.size());
            if (hs < 0) {
                ACE_DEBUG((LM_WARNING,
                           ACE_TEXT("%D httpd:thread:%t %M %N:%l TLS handshake "
                                    "failed\n")));
                return -1;
            }
            if (hs == 0) return 0;  // need more handshake bytes — stay registered
            // hs == 1: handshake done — fall through to read any early app data
        }

        if (m_tls->read_plaintext(app) < 0) {
            return -1;  // close_notify or fatal TLS error
        }
        if (app.empty()) return 0;  // nothing decrypted yet — stay registered
    } else {
        app.assign(buf, static_cast<std::size_t>(n));
    }

    m_parser.feed(app.data(), app.size());

    if (m_parser.error()) {
        ACE_DEBUG((LM_WARNING,
                   ACE_TEXT("%D httpd:thread:%t %M %N:%l parse error: %C\n"),
                   m_parser.error_msg().c_str()));
        const char* bad =
            (m_parser.error_status() == 411)
                ? "HTTP/1.1 411 Length Required\r\n"
                  "Content-Length: 0\r\nConnection: close\r\n\r\n"
                : "HTTP/1.1 400 Bad Request\r\n"
                  "Content-Length: 0\r\nConnection: close\r\n\r\n";
        send_bytes(bad, std::strlen(bad));
        return -1;
    }
    if (!m_parser.done()) return 0;  // partial request — wait for more

    // ── Complete request ──────────────────────────────────────────────
    // Copy it out (the worker outlives this stack frame), capture the
    // keep-alive decision, and ready the parser for the next request.
    HttpParser::Request req = m_parser.request();
    auto it = req.headers.find("connection");
    m_keep_alive = (it != req.headers.end() && it->second == "keep-alive");
    m_parser.reset();

    // ── Auth check (L19/D1) ───────────────────────────────────────────
    // When auth is enabled, protect all /api/v1/* routes except /api/v1/auth/*.
    // 401 responses are short-lived and sent inline even when a worker pool is
    // configured — no point off-loading them.
    if (m_auth && m_auth->enabled() && !is_public_route(req.path)) {
        std::string token = extract_session_cookie(req.headers);
        if (token.empty() || !m_auth->validate(token)) {
            HttpResponse unauth;
            unauth.status = 401;
            unauth.body   = R"({"ok":false,"err":"authentication required"})";
            m_response = unauth.to_string(false);
            return finish_response();
        }
    }

    if (m_pool && m_pool->size() > 0) {
        // Off-load the handler to a worker thread. Suspend reads so no other
        // request (or close) is dispatched on this connection while the
        // worker owns it; the worker routes, stores m_response, and notifies
        // the reactor, which sends from handle_exception() on this thread.
        if (reactor()) reactor()->suspend_handler(this);
        m_pool->submit([this, req = std::move(req)]() mutable {
            auto route_resp = m_router.route(req);
            m_response = route_resp.to_string(m_keep_alive);
            if (reactor()) {
                reactor()->notify(this, ACE_Event_Handler::EXCEPT_MASK);
            }
        });
        return 0;
    }

    // Inline (no pool): route + send on the reactor thread, as before.
    m_response = m_router.route(req).to_string(m_keep_alive);
    return finish_response();
}

int HttpSession::handle_exception(ACE_HANDLE /*fd*/) {
    // A worker finished and notify()'d us; we're back on the reactor thread,
    // so it's safe to touch the socket / TLS engine.
    int rc = finish_response();
    if (rc == 0 && reactor()) {
        reactor()->resume_handler(this);  // keep-alive: re-enable reads
    }
    return rc;  // -1 → reactor removes us → handle_close → delete this
}

int HttpSession::finish_response() {
    send_bytes(m_response.data(), m_response.size());
    m_response.clear();
    m_last_active = std::chrono::steady_clock::now();
    return m_keep_alive ? 0 : -1;
}

void HttpSession::send_bytes(const char* data, std::size_t len) {
    if (m_tls) {
        // Encrypt then flush the resulting ciphertext to the socket.
        if (m_tls->write_plaintext(data, len) == 0) {
            std::string out;
            if (m_tls->drain_outgoing(out)) {
                peer().send_n(out.data(), out.size());
            }
        }
    } else {
        peer().send_n(data, len);
    }
}

int HttpSession::handle_close(ACE_HANDLE /*fd*/, ACE_Reactor_Mask /*mask*/) {
    if (m_tls) {
        // Best-effort close_notify so the client sees a clean shutdown.
        m_tls->shutdown();
        std::string out;
        if (m_tls->drain_outgoing(out)) peer().send_n(out.data(), out.size());
    }
    peer().close();
    delete this;
    return 0;
}

} // namespace http_server
