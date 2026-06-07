#ifndef __data_store_server_server_hpp__
#define __data_store_server_server_hpp__

#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

#include <ace/Event_Handler.h>
#include <ace/LSOCK_Acceptor.h>

#include "data_store.hpp"
#include "worker_pool.hpp"

namespace data_store::server {

class Session;

/// AF_UNIX stream acceptor. Per design.md §2, every accepted
/// connection is wrapped as a Session and registered with the main
/// reactor for reads; reads enqueue ProcessRequest messages onto
/// the session's owner Worker (picked round-robin from the pool at
/// accept time).
///
/// Lifetime is owned by ds-server's main(): one Server + one
/// WorkerPool per process, both opened before the reactor loop
/// starts.
class Server : public ACE_Event_Handler {
public:
    static constexpr mode_t kDefaultMode = 0660;

    Server(std::shared_ptr<DataStore> store,
           WorkerPool*                pool,
           std::string                socketPath);
    ~Server() override;

    /// Bind, chmod, register ACCEPT_MASK with the reactor.
    int open();
    /// Unregister, close fd, unlink the socket path.
    int close();

    int        handle_input(ACE_HANDLE fd) override;
    ACE_HANDLE get_handle() const override;

    const std::string& socket_path() const { return m_socketPath; }

    /// PSK provisioning (task J3) — optionally chgrp the socket to a
    /// shared group after bind, so a static service account (e.g.
    /// `engineer`, member of that group) can connect to a 0660 socket
    /// owned by ds-server's own (DynamicUser) account. Empty = leave the
    /// group as-is. Set before open().
    void set_socket_group(std::string group) {
        m_socketGroup = std::move(group);
    }

    /// Reactor-thread call from Session::handle_close: drop the
    /// session pointer from our registry. The Session itself is
    /// freed by its owner Worker once the SessionClosed message
    /// drains.
    void note_session_closed(Session* s);

private:
    std::shared_ptr<DataStore>      m_store;
    WorkerPool*                     m_pool;
    std::string                     m_socketPath;
    std::string                     m_socketGroup;   ///< J3: shared access group (optional)
    ACE_LSOCK_Acceptor              m_acceptor;
    bool                            m_open = false;
    std::mutex                      m_sessionsMtx;
    std::unordered_set<Session*>    m_sessions;
};

} // namespace data_store::server

#endif /* __data_store_server_server_hpp__ */
