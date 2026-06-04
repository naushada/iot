#ifndef __http_server_session_hpp__
#define __http_server_session_hpp__

#include "parser.hpp"
#include "router.hpp"
#include "tls.hpp"

#include <cstddef>
#include <memory>
#include <string>

#include <ace/Svc_Handler.h>
#include <ace/SOCK_Stream.h>
#include <ace/Synch_Traits.h>

namespace http_server {

class HttpSession
    : public ACE_Svc_Handler<ACE_SOCK_Stream, ACE_MT_SYNCH> {
public:
    /// `tls` non-null → terminate TLS on this connection (https); the
    /// context must outlive every session (owned by main). nullptr → plain
    /// HTTP, the original behaviour.
    explicit HttpSession(const Router& router, const TlsContext* tls = nullptr);

    int handle_input(ACE_HANDLE fd) override;
    int handle_close(ACE_HANDLE fd, ACE_Reactor_Mask mask) override;

private:
    const Router& m_router;
    HttpParser    m_parser;
    std::string   m_recv_buf;

    // TLS engine for this connection (null on a plain-HTTP listener).
    std::unique_ptr<TlsConn> m_tls;

    // Push response bytes to the peer: straight through on plain HTTP, or
    // encrypted via the TLS engine (then drained to the socket) on https.
    void send_bytes(const char* data, std::size_t len);

    void on_request(const HttpParser::Request& req);
};

} // namespace http_server

#endif
