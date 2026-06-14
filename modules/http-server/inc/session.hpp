#ifndef __http_server_session_hpp__
#define __http_server_session_hpp__

#include "parser.hpp"
#include "router.hpp"
#include "tls.hpp"
#include "worker.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

#include <ace/Svc_Handler.h>
#include <ace/SOCK_Stream.h>
#include <ace/Synch_Traits.h>

namespace http_server {

class SessionStore;  // fwd from auth.hpp

class HttpSession
    : public ACE_Svc_Handler<ACE_SOCK_Stream, ACE_MT_SYNCH> {
public:
    /// `tls` non-null → terminate TLS on this connection (https); the
    /// context must outlive every session (owned by main). nullptr → plain
    /// HTTP. `pool` non-null with workers → run handlers off the reactor
    /// thread (FUP-L18-1); null / 0 workers → run inline (the original
    /// behaviour). `auth` non-null → enforce session-auth on protected
    /// routes (L19/D1). All must outlive every session.
    explicit HttpSession(const Router& router,
                         const TlsContext* tls = nullptr,
                         WorkerPool* pool = nullptr,
                         SessionStore* auth = nullptr);

    int handle_input(ACE_HANDLE fd) override;
    int handle_close(ACE_HANDLE fd, ACE_Reactor_Mask mask) override;

    // Send the worker-computed response and re-arm (keep-alive) or tear down
    // the connection. Queued by the worker via WorkerPool::post_to_reactor()
    // and run on the reactor thread by WorkerPool::drain_reactor() — replaces
    // the old notify()/handle_exception() handoff, whose wakeups were lossy.
    void deliver_response();

private:
    const Router& m_router;
    HttpParser    m_parser;
    std::string   m_recv_buf;

    // TLS engine for this connection (null on a plain-HTTP listener).
    std::unique_ptr<TlsConn> m_tls;

    // Handler thread pool (null / 0 workers → inline). Not owned.
    WorkerPool* m_pool = nullptr;

    // Session auth store (null → no auth check). Not owned.
    SessionStore* m_auth = nullptr;

    // Per-request state for the async path. Only one request is in flight
    // per connection (the handler is suspended while a worker runs), so a
    // plain member is race-free: the worker writes m_response then notifies
    // the reactor, which reads it in handle_exception.
    std::string m_response;
    bool        m_keep_alive = false;

    // Idle timeout tracking.  Keep-alive connections that haven't sent a
    // request in kIdleTimeoutSec seconds are closed.  Reset on each request.
    static constexpr long kIdleTimeoutSec = 30;
    std::chrono::steady_clock::time_point m_last_active =
        std::chrono::steady_clock::now();

    // Push response bytes to the peer: straight through on plain HTTP, or
    // encrypted via the TLS engine (then drained to the socket) on https.
    void send_bytes(const char* data, std::size_t len);

    // Send m_response and apply keep-alive: returns 0 to stay registered,
    // -1 to close (reactor then calls handle_close).
    int finish_response();
};

} // namespace http_server

#endif
