#ifndef __http_server_session_hpp__
#define __http_server_session_hpp__

#include "parser.hpp"
#include "router.hpp"

#include <string>

#include <ace/Svc_Handler.h>
#include <ace/SOCK_Stream.h>
#include <ace/MT_SYNCH.h>

namespace http_server {

class HttpSession
    : public ACE_Svc_Handler<ACE_SOCK_Stream, ACE_MT_SYNCH> {
public:
    HttpSession(const Router& router);

    int handle_input(ACE_HANDLE fd) override;
    int handle_close(ACE_HANDLE fd, ACE_Reactor_Mask mask) override;

private:
    const Router& m_router;
    HttpParser    m_parser;
    std::string   m_recv_buf;

    void on_request(const HttpParser::Request& req);
};

} // namespace http_server

#endif
