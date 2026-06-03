#include "http_server/session.hpp"

#include <ace/Log_Msg.h>
#include <ace/Reactor.h>
#include <ace/Time_Value.h>

namespace http_server {

HttpSession::HttpSession(const Router& router)
    : m_router(router) {
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

    m_parser.feed(buf, static_cast<std::size_t>(n));

    if (m_parser.done()) {
        // Send response
        std::string resp = m_parser.take_response();
        peer().send_n(resp.data(), resp.size());

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
        peer().send_n(bad, std::strlen(bad));
        return -1;
    }

    return 0;
}

int HttpSession::handle_close(ACE_HANDLE /*fd*/, ACE_Reactor_Mask /*mask*/) {
    peer().close();
    delete this;
    return 0;
}

} // namespace http_server
