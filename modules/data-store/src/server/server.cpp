#include "server.hpp"

#include "data_store/proto.hpp"
#include "session.hpp"
#include "worker.hpp"

#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include <ace/LSOCK_Stream.h>
#include <ace/Log_Msg.h>
#include <ace/Reactor.h>
#include <ace/UNIX_Addr.h>

namespace data_store::server {

Server::Server(std::shared_ptr<DataStore> store,
               WorkerPool*               pool,
               std::string               socketPath)
  : m_store(std::move(store)),
    m_pool(pool),
    m_socketPath(std::move(socketPath)) {
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
                          ACE_TEXT("%D ds:thread:%t %M %N:%l unlink(%C) "
                                   "failed errno=%d\n"),
                          m_socketPath.c_str(), errno),
                         -1);
    }

    ACE_UNIX_Addr addr(m_socketPath.c_str());
    if (m_acceptor.open(addr) == -1) {
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("%D ds:thread:%t %M %N:%l acceptor.open(%C) "
                                   "failed errno=%d\n"),
                          m_socketPath.c_str(), errno),
                         -1);
    }

    // NFR-DS-004: mode 0660. Group is whatever owns the parent dir;
    // operator-tunable via the systemd unit / installer.
    if (::chmod(m_socketPath.c_str(), kDefaultMode) == -1) {
        ACE_ERROR((LM_WARNING,
                   ACE_TEXT("%D ds:thread:%t %M %N:%l chmod(%C, 0%o) "
                            "failed errno=%d (not fatal)\n"),
                   m_socketPath.c_str(),
                   static_cast<unsigned>(kDefaultMode), errno));
    }

    if (ACE_Reactor::instance()->register_handler(
            this, ACE_Event_Handler::ACCEPT_MASK) == -1) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D ds:thread:%t %M %N:%l register_handler "
                            "failed errno=%d\n"),
                   errno));
        m_acceptor.close();
        ::unlink(m_socketPath.c_str());
        return -1;
    }

    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D ds:thread:%t %M %N:%l listening on %C (mode 0%o)\n"),
               m_socketPath.c_str(),
               static_cast<unsigned>(kDefaultMode)));
    m_open = true;
    return 0;
}

int Server::close() {
    if (!m_open) return 0;
    ACE_Reactor::instance()->remove_handler(
        this, ACE_Event_Handler::ACCEPT_MASK | ACE_Event_Handler::DONT_CALL);

    // Tear down any sessions still alive — reactor-thread shutdown
    // means we own these pointers exclusively. Worker shutdown
    // drains their queues so a session pointer can be freed here
    // safely (no in-flight DeliverNotify can race).
    std::unordered_set<Session*> alive;
    {
        std::lock_guard<std::mutex> g(m_sessionsMtx);
        alive.swap(m_sessions);
    }
    for (Session* s : alive) {
        ACE_Reactor::instance()->remove_handler(
            s, ACE_Event_Handler::READ_MASK |
               ACE_Event_Handler::DONT_CALL);
        delete s;
    }

    m_acceptor.close();
    ::unlink(m_socketPath.c_str());
    m_open = false;
    return 0;
}

int Server::handle_input(ACE_HANDLE /*fd*/) {
    // Reactor thread: accept, pin to a worker, register the session
    // with the reactor for reads. Session::handle_input → enqueue
    // request lines to the owner worker for parsing + dispatch.
    ACE_LSOCK_Stream stream;
    if (m_acceptor.accept(stream) == -1) {
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("%D ds:thread:%t %M %N:%l accept "
                                   "failed errno=%d\n"),
                          errno),
                         0);     // stay registered
    }

    Worker* w = m_pool ? m_pool->next() : nullptr;
    if (!w) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D ds:thread:%t %M %N:%l no worker pool; "
                            "dropping connection\n")));
        stream.close();
        return 0;
    }

    auto* s = new Session(stream, w, m_store.get(), this);
    {
        std::lock_guard<std::mutex> g(m_sessionsMtx);
        m_sessions.insert(s);
    }

    // EMP has no welcome handshake — the server is ready to receive
    // frames as soon as the socket is registered with the reactor.
    if (ACE_Reactor::instance()->register_handler(
            s, ACE_Event_Handler::READ_MASK) == -1) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D ds:thread:%t %M %N:%l register_handler for "
                            "session failed errno=%d\n"),
                   errno));
        {
            std::lock_guard<std::mutex> g(m_sessionsMtx);
            m_sessions.erase(s);
        }
        delete s;
        return 0;
    }
    return 0;
}

void Server::note_session_closed(Session* s) {
    if (!s) return;
    std::lock_guard<std::mutex> g(m_sessionsMtx);
    m_sessions.erase(s);
}

ACE_HANDLE Server::get_handle() const {
    return const_cast<ACE_LSOCK_Acceptor&>(m_acceptor).get_handle();
}

} // namespace data_store::server
