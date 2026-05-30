#ifndef __data_store_server_server_hpp__
#define __data_store_server_server_hpp__

#include <memory>
#include <string>

#include <ace/Event_Handler.h>
#include <ace/LSOCK_Acceptor.h>

#include "data_store.hpp"

namespace data_store::server {

/// AF_UNIX stream acceptor. Per the design (modules/data-store/docs/
/// design.md §2), every accepted connection becomes a Session; D1
/// stops there and writes a single welcome line before closing.
///
/// Lifetime is owned by ds-server's main(): one Server per
/// process, registered with the ACE_Reactor singleton.
class Server : public ACE_Event_Handler {
public:
    static constexpr mode_t kDefaultMode = 0660;

    Server(std::shared_ptr<DataStore> store, std::string socketPath);
    ~Server() override;

    /// Bind, chmod, register READ_MASK with the reactor.
    int open();
    /// Unregister, close fd, unlink the socket path.
    int close();

    int        handle_input(ACE_HANDLE fd) override;
    ACE_HANDLE get_handle() const override;

    const std::string& socket_path() const { return m_socketPath; }

private:
    std::shared_ptr<DataStore> m_store;
    std::string                m_socketPath;
    ACE_LSOCK_Acceptor         m_acceptor;
    bool                       m_open = false;
};

} // namespace data_store::server

#endif /* __data_store_server_server_hpp__ */
