#ifndef __udp_adapter_hpp__
#define __udp_adapter_hpp__

#include <vector>
#include <unordered_map>
#include <iostream>
#include <memory>
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <cerrno>
#include <sstream>
#include <deque>
#include <atomic>
#include <functional>
#include <mutex>

#include <ace/Event_Handler.h>
#include <ace/Reactor.h>
#include <ace/Task.h>
#include <ace/SOCK_Dgram.h>
#include <ace/INET_Addr.h>
#include <ace/Sig_Handler.h>
#include <ace/Synch_Traits.h>

#include "dtls_adapter.hpp"
#include "coap_adapter.hpp"

extern "C"
{
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <arpa/inet.h>
    #include <sys/types.h>
    #include <sys/un.h>
    #include <netdb.h>
    #include <signal.h>
}

class UDPAdapter;

/**
 * @brief One UDP socket bound to (host, port), registered with the ACE reactor.
 *
 * Replaces the per-service epoll context. A single instance owns the
 * ACE_SOCK_Dgram, the DTLS context (if scheme == CoAPs), and the CoAP
 * adapter; per-peer DTLS state continues to live inside the DTLSAdapter's
 * client table.
 *
 * Inbound: ACE invokes handle_input() when bytes arrive; we recv into a
 * fixed buffer and either feed tinydtls (CoAPs) or push directly into the
 * CoAP adapter (CoAP).
 *
 * Outbound: callers post work via UDPAdapter::send_async(), which queues
 * the payload and wakes the reactor via notify(). The reactor thread then
 * drains the queue in handle_exception() — this is the only place
 * dtls_write() is invoked, so tinydtls is touched from a single thread.
 */
class ServiceContext_t : public ACE_Event_Handler {
public:
    typedef enum {
        CoAPs = 1,
        CoAP = 2,
        INVALID = 3
    } Scheme_t;

    typedef enum {
        DeviceMgmtServer  = 0,
        BootsstrapServer  = 1,
        DeviceMgmtClient  = 3,
        LwM2MClient       = 4
    } ServiceType_t;

    struct OutboundFrame {
        std::string                 payload;
        bool                        haveExplicitPeer;
        std::string                 peerHost;
        std::uint16_t               peerPort;
    };

    ServiceContext_t(Scheme_t scheme,
                     ServiceType_t service,
                     const std::string& host,
                     std::uint16_t port);
    ServiceContext_t() = delete;
    ~ServiceContext_t() override;

    /// ACE_Event_Handler overrides
    ACE_HANDLE get_handle() const override;
    int handle_input(ACE_HANDLE handle) override;
    int handle_exception(ACE_HANDLE handle) override;
    int handle_close(ACE_HANDLE handle, ACE_Reactor_Mask mask) override;

    /// Bind the socket and register with the reactor.
    int open();

    /// Thread-safe enqueue: any thread can call this. The payload is sent
    /// to the configured peer (or the explicit peer, when provided) on the
    /// reactor thread.
    int send_async(const std::string& payload);
    int send_async(const std::string& payload,
                   const std::string& peerHost,
                   std::uint16_t peerPort);

    /// Used by tinydtls write callback to push ciphertext on the wire.
    /// MUST be invoked on the reactor thread.
    int send_raw(const ACE_INET_Addr& to, const std::uint8_t* data, std::size_t len);

    void peerHost(std::string host) { m_peerHost = std::move(host); }
    std::string& peerHost()         { return m_peerHost; }

    void peerPort(std::uint16_t port) { m_peerPort = port; }
    std::uint16_t& peerPort()         { return m_peerPort; }

    void selfHost(std::string host) { m_selfHost = std::move(host); }
    std::string& selfHost()         { return m_selfHost; }

    void selfPort(std::uint16_t port) { m_selfPort = port; }
    std::uint16_t& selfPort()         { return m_selfPort; }

    void scheme(Scheme_t sc) { m_scheme = sc; }
    Scheme_t& scheme()       { return m_scheme; }

    void service(ServiceType_t sc) { m_service = sc; }
    ServiceType_t& service()       { return m_service; }

    std::int32_t fd() { return static_cast<std::int32_t>(m_sock.get_handle()); }

    std::shared_ptr<DTLSAdapter>& dtlsAdapter() { return m_dtlsAdapter; }
    std::shared_ptr<CoAPAdapter>& coapAdapter() { return m_coapAdapter; }

    ACE_SOCK_Dgram& sock() { return m_sock; }

private:
    /// Drain m_txQueue under the reactor thread, calling either the DTLS
    /// path or the raw UDP path depending on the scheme.
    void drain_tx_queue();

    /// CoAP-plaintext receive path.
    void handle_input_coap(const std::string& bytes, const ACE_INET_Addr& from);
    /// CoAPs / DTLS receive path.
    void handle_input_coaps(const std::vector<std::uint8_t>& bytes, const ACE_INET_Addr& from);

    ACE_SOCK_Dgram                 m_sock;
    ACE_INET_Addr                  m_selfAddr;
    std::string                    m_peerHost;
    std::uint16_t                  m_peerPort{0};
    std::string                    m_selfHost;
    std::uint16_t                  m_selfPort{0};
    Scheme_t                       m_scheme;
    ServiceType_t                  m_service;
    std::shared_ptr<CoAPAdapter>   m_coapAdapter;
    std::shared_ptr<DTLSAdapter>   m_dtlsAdapter;

    std::mutex                     m_txMutex;
    std::deque<OutboundFrame>      m_txQueue;
};

/**
 * @brief Owns the set of UDP services and (on the client) the reactor
 * worker thread.
 *
 * Server role: main() drives the reactor on the main thread; UDPAdapter is
 * just a container for the ServiceContexts.
 *
 * Client role: UDPAdapter::start() is invoked on a dedicated ACE_Task
 * thread (svc()) that runs the reactor event loop while the main thread
 * blocks in Readline.
 */
class UDPAdapter : public ACE_Task<ACE_MT_SYNCH> {
public:
    using Scheme_t      = ServiceContext_t::Scheme_t;
    using ServiceType_t = ServiceContext_t::ServiceType_t;
    using Role_t        = enum { SERVER = 1, CLIENT = 2 };

    UDPAdapter(std::string& host, std::uint16_t& port, Scheme_t& scheme, ServiceType_t& service);
    UDPAdapter() = delete;
    ~UDPAdapter() override;

    /// Bind a new UDP service.
    int init(const std::string& host, const std::uint16_t& port,
             const Scheme_t& scheme, const ServiceType_t& service);
    int init(const std::string& host, const std::uint16_t& port,
             const Scheme_t& scheme);

    /// Register an already-init'd service with the reactor.
    int add_event_handle(const Scheme_t& scheme, const ServiceType_t& svc);

    /// Drive the reactor. On the server this blocks the calling thread;
    /// on the client it activates an ACE_Task worker that drives the
    /// reactor and returns immediately.
    int start(Role_t role, Scheme_t scheme);
    int stop();

    /// Outbound CoAP/CoAPs frame originating from the readline thread.
    /// Looks up the service and forwards to ServiceContext_t::send_async,
    /// which is thread-safe.
    int tx(std::string& payload, ServiceType_t& service);

    std::unordered_map<ServiceType_t, std::unique_ptr<ServiceContext_t>>& services() {
        return m_services;
    }

    /// ACE_Task overrides — used only for client-mode reactor thread.
    int open(void* args = nullptr) override;
    int svc() override;

    /// 1 Hz tick fired on the reactor thread once start() has registered
    /// the periodic ACE_Reactor::schedule_timer. Used by L3+ for the
    /// Registration lifetime ticker (server-side expire / client-side
    /// Update) and by L7 for the Observe pmax timer.
    int handle_timeout(const ACE_Time_Value& tv, const void* act) override;

    /// Install the L3 server-side tick callback (registry expire +
    /// mirror Remove events). Called once at startup; no thread safety
    /// because the reactor isn't running yet.
    void on_tick_server(std::function<void()> cb) { m_tickServer = std::move(cb); }

    /// Install the L3 client-side tick callback (RegistrationClient::
    /// should_send_update + Update enqueue).
    void on_tick_client(std::function<void()> cb) { m_tickClient = std::move(cb); }

private:
    std::unordered_map<ServiceType_t, std::unique_ptr<ServiceContext_t>> m_services;
    Role_t m_role{SERVER};
    std::atomic<bool> m_stop{false};
    std::function<void()> m_tickServer;
    std::function<void()> m_tickClient;
};

#endif /*__udp_adapter_hpp__*/
