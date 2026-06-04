#include "http_server/session.hpp"

#include <ace/Log_Msg.h>
#include <ace/Reactor.h>
#include <ace/Time_Value.h>

#include <cstring>
#include <memory>

namespace http_server {

HttpSession::HttpSession(const Router& router, const TlsContext* tls)
    : m_router(router) {
    if (tls && *tls) {
        m_tls = std::make_unique<TlsConn>(*tls);
    }
    m_parser.set_handler([this](const HttpParser::Request& req) {
        auto resp = m_router.route(req);
        return resp.to_string();
    });
}

int HttpSession::handle_input(ACE_HANDLE /*fd*/) {
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
                           ACE_TEXT("%D [http:%t] %M %N:%l TLS handshake "
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

    if (m_parser.done()) {
        std::string resp = m_parser.take_response();
        send_bytes(resp.data(), resp.size());

        // Check keep-alive
        bool keep_alive = false;
        auto it = m_parser.request().headers.find("connection");
        if (it != m_parser.request().headers.end() &&
            it->second == "keep-alive") {
            keep_alive = true;
        }

        if (keep_alive) {
            m_parser.reset();
        } else {
            return -1;   // close after response
        }
    } else if (m_parser.error()) {
        ACE_DEBUG((LM_WARNING,
                   ACE_TEXT("%D [http:%t] %M %N:%l parse error: %C\n"),
                   m_parser.error_msg().c_str()));
        // Send 400
        const char* bad = "HTTP/1.1 400 Bad Request\r\n"
                          "Content-Length: 0\r\n"
                          "Connection: close\r\n\r\n";
        send_bytes(bad, std::strlen(bad));
        return -1;
    }

    return 0;
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
