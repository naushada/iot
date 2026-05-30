#ifndef __udp_adapter_cpp__
#define __udp_adapter_cpp__

#include "udp_adapter.hpp"

#include <ace/Log_Msg.h>
#include <ace/Reactor.h>
#include <ace/Signal.h>          // ACE_Sig_Set
#include <ace/Time_Value.h>

/* ───────────────────────── ServiceContext_t ─────────────────────────── */

ServiceContext_t::ServiceContext_t(Scheme_t scheme,
                                   ServiceType_t service,
                                   const std::string& host,
                                   std::uint16_t port)
    : m_selfAddr(port, host.c_str()),
      m_selfHost(host),
      m_selfPort(port),
      m_scheme(scheme),
      m_service(service),
      m_coapAdapter(std::make_shared<CoAPAdapter>()) {
    if (m_scheme == Scheme_t::CoAPs) {
        // DTLSAdapter takes the raw fd because tinydtls' read/write
        // callbacks ::sendto/::recvfrom on it directly. The fd is filled
        // in inside open() once the socket is actually bound.
        m_dtlsAdapter = std::make_shared<DTLSAdapter>(ACE_INVALID_HANDLE, DTLS_LOG_DEBUG);
    }
}

ServiceContext_t::~ServiceContext_t() {
    if (m_sock.get_handle() != ACE_INVALID_HANDLE) {
        ACE_DEBUG((LM_DEBUG,
                   ACE_TEXT("%D [UdpSvc:%t] %M %N:%l closing fd:%d service:%d\n"),
                   m_sock.get_handle(), m_service));
        m_sock.close();
    }
}

int ServiceContext_t::open() {
    if (m_sock.open(m_selfAddr) == -1) {
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("%D [UdpSvc:%t] %M %N:%l bind failed host:%s port:%d errno:%d\n"),
                          m_selfHost.c_str(), m_selfPort, errno),
                         -1);
    }

    if (m_scheme == Scheme_t::CoAPs && m_dtlsAdapter) {
        // Patch in the bound fd so tinydtls' send callback can ::sendto.
        // DTLSAdapter exposes its dtlsFd via getFd(); we update it via the
        // session() helper isn't appropriate here, so reach the field via
        // a dedicated setter added in dtls_adapter.hpp.
        m_dtlsAdapter->setFd(m_sock.get_handle());
    }

    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D [UdpSvc:%t] %M %N:%l bound fd:%d host:%s port:%d scheme:%d service:%d\n"),
               m_sock.get_handle(), m_selfHost.c_str(), m_selfPort, m_scheme, m_service));
    return 0;
}

ACE_HANDLE ServiceContext_t::get_handle() const {
    return const_cast<ACE_SOCK_Dgram&>(m_sock).get_handle();
}

int ServiceContext_t::handle_input(ACE_HANDLE /*handle*/) {
    if (m_scheme == Scheme_t::CoAPs) {
        // Don't consume the datagram here — DTLSAdapter::rx() does its own
        // recvfrom and then drives dtls_handle_message(). The DTLS read
        // callback in turn fills in m_peerHost / m_peerPort via session_t.
        handle_input_coaps(/*unused*/ {}, ACE_INET_Addr{});
        return 0;
    }

    std::vector<std::uint8_t> buf(2048);
    ACE_INET_Addr from;
    ssize_t n = m_sock.recv(buf.data(), buf.size(), from);
    if (n < 0) {
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("%D [UdpSvc:%t] %M %N:%l recv failed errno:%d\n"),
                          errno),
                         0);
    }
    buf.resize(static_cast<std::size_t>(n));

    char host[64] = {0};
    from.get_host_addr(host, sizeof(host));
    m_peerHost.assign(host);
    m_peerPort = from.get_port_number();

    std::string bytes(reinterpret_cast<const char*>(buf.data()), buf.size());
    handle_input_coap(bytes, from);
    return 0;
}

void ServiceContext_t::handle_input_coap(const std::string& bytes,
                                         const ACE_INET_Addr& from) {
    CoAPAdapter::CoAPMessage message;
    m_coapAdapter->parseRequest(bytes, message);

    // L9 fix: every service routes through processRequest so the
    // attached handlers (m_dmClient on LwM2MClient, m_bsServer on the
    // BootstrapServer, m_regServer on the DeviceMgmtServer) get first
    // refusal and the ACK-echo branch (don't reply to ACKs) engages.
    // The previous LwM2MClient path called buildResponse directly,
    // which round-tripped a spurious 2.04 Changed back to whoever sent
    // us an ACK (frame 3 in nfr-001-coap.pcap).
    std::vector<std::string> responses;
    const bool isClient = (m_service == ServiceType_t::LwM2MClient) ||
                          (m_service == ServiceType_t::DeviceMgmtClient);
    m_coapAdapter->processRequest(isClient, bytes, responses);

    char host[64] = {0};
    from.get_host_addr(host, sizeof(host));
    for (auto& rsp : responses) {
        ssize_t sent = m_sock.send(rsp.data(), rsp.size(), from);
        if (sent < 0) {
            ACE_ERROR((LM_ERROR,
                       ACE_TEXT("%D [UdpSvc:%t] %M %N:%l CoAP send failed to %s:%d errno:%d\n"),
                       host, from.get_port_number(), errno));
        }
    }
}

void ServiceContext_t::handle_input_coaps(const std::vector<std::uint8_t>& bytes,
                                          const ACE_INET_Addr& /*from*/) {
    if (!m_dtlsAdapter) {
        return;
    }
    // Feed tinydtls. tinydtls' read callback will populate
    // m_dtlsAdapter->responses(); we forward those to the wire here so
    // there is no shared queue across threads.
    m_dtlsAdapter->rx(m_sock.get_handle());
    for (auto& rsp : m_dtlsAdapter->responses()) {
        m_dtlsAdapter->tx(rsp);
    }
    m_dtlsAdapter->responses(std::vector<std::string>{});
}

int ServiceContext_t::handle_exception(ACE_HANDLE /*handle*/) {
    drain_tx_queue();
    return 0;
}

void ServiceContext_t::drain_tx_queue() {
    std::deque<OutboundFrame> local;
    {
        std::lock_guard<std::mutex> lock(m_txMutex);
        local.swap(m_txQueue);
    }

    for (auto& frame : local) {
        std::string host = frame.haveExplicitPeer ? frame.peerHost : m_peerHost;
        std::uint16_t port = frame.haveExplicitPeer ? frame.peerPort : m_peerPort;

        if (host.empty() || port == 0) {
            ACE_ERROR((LM_ERROR,
                       ACE_TEXT("%D [UdpSvc:%t] %M %N:%l drop tx, no peer for service:%d\n"),
                       m_service));
            continue;
        }

        if (m_scheme == Scheme_t::CoAPs && m_dtlsAdapter) {
            if (m_dtlsAdapter->clientState() != "connected") {
                m_dtlsAdapter->connect(host, port);
            }
            if (m_dtlsAdapter->clientState() == "connected") {
                m_dtlsAdapter->tx(frame.payload);
            } else {
                ACE_DEBUG((LM_DEBUG,
                           ACE_TEXT("%D [UdpSvc:%t] %M %N:%l DTLS not connected, dropping tx\n")));
            }
        } else {
            ACE_INET_Addr to(port, host.c_str());
            ssize_t sent = m_sock.send(frame.payload.data(), frame.payload.size(), to);
            if (sent < 0) {
                ACE_ERROR((LM_ERROR,
                           ACE_TEXT("%D [UdpSvc:%t] %M %N:%l raw UDP send failed errno:%d\n"),
                           errno));
            }
        }
    }
}

int ServiceContext_t::send_async(const std::string& payload) {
    {
        std::lock_guard<std::mutex> lock(m_txMutex);
        m_txQueue.push_back({payload, false, {}, 0});
    }
    ACE_Reactor::instance()->notify(this);
    return 0;
}

int ServiceContext_t::send_async(const std::string& payload,
                                 const std::string& host,
                                 std::uint16_t port) {
    {
        std::lock_guard<std::mutex> lock(m_txMutex);
        m_txQueue.push_back({payload, true, host, port});
    }
    ACE_Reactor::instance()->notify(this);
    return 0;
}

int ServiceContext_t::send_raw(const ACE_INET_Addr& to,
                               const std::uint8_t* data, std::size_t len) {
    return static_cast<int>(m_sock.send(data, len, to));
}

int ServiceContext_t::handle_close(ACE_HANDLE /*handle*/, ACE_Reactor_Mask /*mask*/) {
    if (m_sock.get_handle() != ACE_INVALID_HANDLE) {
        m_sock.close();
    }
    return 0;
}

/* ───────────────────────────── UDPAdapter ───────────────────────────── */

UDPAdapter::UDPAdapter(std::string& host, std::uint16_t& port,
                       Scheme_t& scheme, ServiceType_t& service) {
    init(host, port, scheme, service);
}

UDPAdapter::~UDPAdapter() {
    stop();
    // unique_ptr<ServiceContext_t> destructors close fds and remove
    // themselves from the reactor.
    m_services.clear();
}

int UDPAdapter::init(const std::string& host, const std::uint16_t& port,
                     const Scheme_t& scheme, const ServiceType_t& service) {
    auto ctx = std::make_unique<ServiceContext_t>(scheme, service, host, port);
    if (ctx->open() == -1) {
        return -1;
    }
    if (!m_services.insert({service, std::move(ctx)}).second) {
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("%D [UDPAdapter:%t] %M %N:%l duplicate service:%d\n"),
                          service),
                         -1);
    }
    return 0;
}

int UDPAdapter::init(const std::string& host, const std::uint16_t& port,
                     const Scheme_t& scheme) {
    return init(host, port, scheme, ServiceType_t::LwM2MClient);
}

int UDPAdapter::add_event_handle(const Scheme_t& /*scheme*/, const ServiceType_t& svc) {
    auto it = m_services.find(svc);
    if (it == m_services.end()) {
        return -1;
    }
    if (ACE_Reactor::instance()->register_handler(
            it->second.get(),
            ACE_Event_Handler::READ_MASK) == -1) {
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("%D [UDPAdapter:%t] %M %N:%l register_handler "
                                   "failed for service:%d errno:%d\n"),
                          svc, errno),
                         -1);
    }
    return 0;
}

int UDPAdapter::tx(std::string& payload, ServiceType_t& service) {
    auto it = m_services.find(service);
    if (it == m_services.end()) {
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("%D [UDPAdapter:%t] %M %N:%l tx: unknown service:%d\n"),
                          service),
                         -1);
    }
    return it->second->send_async(payload);
}

int UDPAdapter::start(Role_t role, Scheme_t scheme) {
    m_role = role;

    if (role == CLIENT && scheme == Scheme_t::CoAPs) {
        // Kick off the DTLS handshake against the bootstrap server.
        auto it = std::find_if(m_services.begin(), m_services.end(),
                               [](const auto& kv) {
                                   return kv.second->service() == ServiceType_t::LwM2MClient;
                               });
        if (it != m_services.end()) {
            auto& ctx = it->second;
            ctx->dtlsAdapter()->connect(ctx->peerHost(), ctx->peerPort());
        }
    }

    // L3 / REQ-IO-005: 1 Hz periodic timer drives lifetime + Update ticks
    // and (later) the Observe pmax timer. Scheduled here so it is alive
    // for both server (main-thread reactor) and client (ACE_Task reactor)
    // modes. First tick after 1 s; subsequent every 1 s.
    ACE_Time_Value delay(1, 0);
    ACE_Time_Value interval(1, 0);
    ACE_Reactor::instance()->schedule_timer(this, nullptr, delay, interval);

    // Both roles spawn the reactor on its own ACE_Task thread so the
    // main thread is free to host the Readline REPL (or block on
    // wait() in non-TTY runs). The server path used to run the
    // reactor inline on main(); pushing it onto svc() unifies the two
    // and unlocks the CLI on server binaries.
    ::signal(SIGPIPE, SIG_IGN);
    return open();
}

int UDPAdapter::handle_timeout(const ACE_Time_Value& /*tv*/, const void* /*act*/) {
    // Fired by ACE on the reactor thread, so callbacks may freely touch
    // ServiceContext / DTLSAdapter / ClientRegistry state without locks.
    if (m_role == SERVER && m_tickServer) {
        m_tickServer();
    } else if (m_role == CLIENT && m_tickClient) {
        m_tickClient();
    }
    return 0;
}

int UDPAdapter::stop() {
    m_stop.store(true);
    ACE_Reactor::instance()->end_reactor_event_loop();
    msg_queue()->deactivate();
    wait();
    return 0;
}

int UDPAdapter::open(void* /*args*/) {
    if (activate(THR_NEW_LWP | THR_JOINABLE, 1) == -1) {
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("%D [UDPAdapter:%t] %M %N:%l activate failed\n")),
                         -1);
    }
    return 0;
}

int UDPAdapter::svc() {
    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D [UDPAdapter:%t] %M %N:%l reactor thread started\n")));

    ::signal(SIGPIPE, SIG_IGN);

    // ACE_Select_Reactor requires the calling thread to own the reactor
    // before handle_events() will dispatch. Without this, handle_events
    // returns -1 instantly from a worker thread and svc() exits before
    // it processes any datagram. Both client and server now go through
    // this svc() path (see UDPAdapter::start) so the owner() call is
    // unconditional.
    ACE_Reactor::instance()->owner(ACE_Thread::self());

    ACE_Time_Value to(1, 0);
    while (!m_stop.load()) {
        int ret = ACE_Reactor::instance()->handle_events(to);
        if (ret < 0) {
            ACE_DEBUG((LM_DEBUG,
                       ACE_TEXT("%D [UDPAdapter:%t] %M %N:%l "
                                "handle_events ret=%d errno=%d\n"),
                       ret, errno));
            break;
        }
    }
    ACE_DEBUG((LM_DEBUG,
               ACE_TEXT("%D [UDPAdapter:%t] %M %N:%l reactor thread exiting\n")));
    return 0;
}

#endif /*__udp_adapter_cpp__*/
