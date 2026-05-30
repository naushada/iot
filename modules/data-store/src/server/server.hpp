#ifndef __data_store_server_server_hpp__
#define __data_store_server_server_hpp__

#include <memory>
#include <string>

#include <ace/Event_Handler.h>
#include <ace/LSOCK_Acceptor.h>

#include "data_store.hpp"
#include "worker_pool.hpp"

namespace data_store::server {

/// AF_UNIX stream acceptor. Per design.md §2, every accepted
/// connection is wrapped as a WorkRequest and handed to the next
/// Worker in a round-robin pool of N (default 5) active objects.
/// D1 worker bodies write a welcome line and close; D2+ replaces
/// that body with the JSON op dispatcher.
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

private:
    std::shared_ptr<DataStore> m_store;
    WorkerPool*                m_pool;
    std::string                m_socketPath;
    ACE_LSOCK_Acceptor         m_acceptor;
    bool                       m_open = false;
};

} // namespace data_store::server

#endif /* __data_store_server_server_hpp__ */
