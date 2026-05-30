#include "server.hpp"

#include "data_store/proto.hpp"

#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include <ace/LSOCK_Stream.h>
#include <ace/Log_Msg.h>
#include <ace/Reactor.h>
#include <ace/UNIX_Addr.h>

namespace data_store::server {

Server::Server(std::shared_ptr<DataStore> store, std::string socketPath)
  : m_store(std::move(store)), m_socketPath(std::move(socketPath)) {
}

Server::~Server() {
    close();
}

int Server::open() {
    if (m_open) return 0;

    // Best-effort: a stale socket from a prior crash trips bind with
    // EADDRINUSE. ENOENT is fine — file simply doesn't exist yet.
    if (::unlink(m_socketPath.c_str()) == -1 && errno != ENOENT) {
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("%D [DS:%t] %M %N:%l unlink(%C) "
                                   "failed errno=%d\n"),
                          m_socketPath.c_str(), errno),
                         -1);
    }

    ACE_UNIX_Addr addr(m_socketPath.c_str());
    if (m_acceptor.open(addr) == -1) {
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("%D [DS:%t] %M %N:%l acceptor.open(%C) "
                                   "failed errno=%d\n"),
                          m_socketPath.c_str(), errno),
                         -1);
    }

    // NFR-DS-004: mode 0660. Group is whatever owns the parent dir;
    // operator-tunable via the systemd unit / installer.
    if (::chmod(m_socketPath.c_str(), kDefaultMode) == -1) {
        ACE_ERROR((LM_WARNING,
                   ACE_TEXT("%D [DS:%t] %M %N:%l chmod(%C, 0%o) "
                            "failed errno=%d (not fatal)\n"),
                   m_socketPath.c_str(),
                   static_cast<unsigned>(kDefaultMode), errno));
    }

    if (ACE_Reactor::instance()->register_handler(
            this, ACE_Event_Handler::ACCEPT_MASK) == -1) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [DS:%t] %M %N:%l register_handler "
                            "failed errno=%d\n"),
                   errno));
        m_acceptor.close();
        ::unlink(m_socketPath.c_str());
        return -1;
    }

    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D [DS:%t] %M %N:%l listening on %C (mode 0%o)\n"),
               m_socketPath.c_str(),
               static_cast<unsigned>(kDefaultMode)));
    m_open = true;
    return 0;
}

int Server::close() {
    if (!m_open) return 0;
    ACE_Reactor::instance()->remove_handler(
        this, ACE_Event_Handler::ACCEPT_MASK | ACE_Event_Handler::DONT_CALL);
    m_acceptor.close();
    ::unlink(m_socketPath.c_str());
    m_open = false;
    return 0;
}

int Server::handle_input(ACE_HANDLE /*fd*/) {
    // D1: accept → welcome → close. The session class with
    // line-delimited JSON parsing lands in D2.
    ACE_LSOCK_Stream stream;
    if (m_acceptor.accept(stream) == -1) {
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("%D [DS:%t] %M %N:%l accept "
                                   "failed errno=%d\n"),
                          errno),
                         0);     // stay registered
    }

    const std::size_t len = std::strlen(proto::kWelcomeLine);
    ssize_t wrote = stream.send_n(proto::kWelcomeLine, len);
    if (wrote < 0 || static_cast<std::size_t>(wrote) != len) {
        ACE_ERROR((LM_WARNING,
                   ACE_TEXT("%D [DS:%t] %M %N:%l welcome short write "
                            "(%d/%d) errno=%d\n"),
                   static_cast<int>(wrote),
                   static_cast<int>(len),
                   errno));
    }
    stream.close();
    return 0;
}

ACE_HANDLE Server::get_handle() const {
    return const_cast<ACE_LSOCK_Acceptor&>(m_acceptor).get_handle();
}

} // namespace data_store::server
