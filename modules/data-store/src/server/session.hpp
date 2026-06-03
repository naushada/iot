#ifndef __data_store_server_session_hpp__
#define __data_store_server_session_hpp__

/// Per-connection state. Lives from accept() to the moment the
/// remote peer closes the socket OR the server tears down.
///
/// Reads live on the main reactor thread (handle_input → enqueue
/// onto owner Worker). Writes live on the owner Worker thread
/// (response to a request OR a notify delivery). The single-reader /
/// single-writer split means no socket-level locking is required.
///
/// Session lifetime:
///   - Server::handle_input news the Session, registers it with the
///     reactor, hands ownership to the SessionRegistry kept by Server.
///   - On EOF / I/O error, the reactor removes the handler and
///     enqueues a SessionClosed message to the Worker so it can
///     unwatch + delete the Session.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>

#include <ace/Event_Handler.h>
#include <ace/LSOCK_Stream.h>

namespace data_store::server {

class DataStore;
class Worker;
class Server;

class Session : public ACE_Event_Handler {
public:
    Session(ACE_LSOCK_Stream stream,
            Worker*          owner,
            DataStore*       store,
            Server*          server);

    /// ACE reactor hook: byte(s) ready on this session's fd.
    int handle_input(ACE_HANDLE fd) override;
    /// Reactor close hook (also called on remove_handler).
    int handle_close(ACE_HANDLE fd, ACE_Reactor_Mask mask) override;
    ACE_HANDLE get_handle() const override;

    /// Worker-thread write. Returns 0 on success, -1 on write error
    /// (typically EPIPE — peer hung up). Caller MUST be the owner
    /// Worker; cross-Worker callers should enqueue a DeliverNotify
    /// message instead so the owner Worker performs the write.
    int send(const std::string& bytes);

    Worker*    owner()    const { return m_owner; }
    DataStore* store()    const { return m_store; }

    /// L17c — peer credentials captured at accept() time via
    /// SO_PEERCRED (Linux) or getpeereid() (macOS). uid=0 means
    /// root; gid is the primary group.
    std::uint32_t peer_uid() const { return m_uid; }
    std::uint32_t peer_gid() const { return m_gid; }

    /// Watch-set bookkeeping mirrors what DataStore knows so the
    /// reactor thread can fast-clean on disconnect without locking.
    /// Mutated only by the owner Worker.
    void note_watch(const std::string& k);
    void note_unwatch(const std::string& k);
    const std::unordered_set<std::string>& watches() const { return m_watches; }

    /// Reactor-thread call after handle_close: tells the server it
    /// can drop the session pointer from its registry.
    void mark_closed_in_reactor() { m_closed_in_reactor = true; }
    bool closed_in_reactor() const { return m_closed_in_reactor; }

    // Recv buffer (only touched on the reactor thread).
    std::string& recv_buf() { return m_recvBuf; }

private:
    ACE_LSOCK_Stream                m_stream;
    Worker*                         m_owner;
    DataStore*                      m_store;
    Server*                         m_server;
    std::uint32_t                   m_uid = 0xFFFFFFFFu;  // unknown (L17c)
    std::uint32_t                   m_gid = 0xFFFFFFFFu;  // unknown (L17c)
    std::string                     m_recvBuf;
    std::unordered_set<std::string> m_watches;
    bool                            m_closed_in_reactor = false;
};

} // namespace data_store::server

#endif /* __data_store_server_session_hpp__ */
