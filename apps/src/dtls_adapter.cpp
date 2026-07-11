#ifndef __dtls_adapter_cpp__
#define __dtls_adapter_cpp__

// data-store headers MUST precede dtls_adapter.hpp: tinydtls' numeric.h (pulled
// in transitively by dtls_adapter.hpp) #defines a max(A,B) macro that otherwise
// clobbers std::numeric_limits<>::max() inside data_store/value.hpp.
#include "data_store/client.hpp"
#include "data_store/value.hpp"

#include "dtls_adapter.hpp"

#include <ace/Log_Msg.h>
#include <ace/OS_NS_sys_time.h>   // ACE_OS::gettimeofday for the reaper clock
#include <cctype>
#include <cstdint>  // std::uint64_t (reaper timestamps)
#include <cstdio>   // keylog dump (fopen/fprintf) — see dtlsGetPskInfoCb
#include <cstdlib>  // getenv($IOT_DTLS_KEYLOG)

namespace {
    /// Wall-clock milliseconds via ACE (project rule: ACE over raw POSIX).
    std::uint64_t dtls_now_ms() {
        ACE_Time_Value tv = ACE_OS::gettimeofday();
        return static_cast<std::uint64_t>(tv.sec()) * 1000ull
             + static_cast<std::uint64_t>(tv.usec()) / 1000ull;
    }
    /// "ip:port" for a session (IPv4 — session() only ever builds sockaddr_in).
    std::string dtls_session_key(const session_t& s) {
        struct in_addr a; a.s_addr = s.addr.sin.sin_addr.s_addr;
        char buf[32];
        std::snprintf(buf, sizeof buf, "%s:%u", inet_ntoa(a),
                      static_cast<unsigned>(ntohs(s.addr.sin.sin_port)));
        return std::string(buf);
    }
    /// A server DTLS handshake reaches CONNECTED within a few seconds even over a
    /// lossy 2G link. One still half-open past this is wedged (see the reaper
    /// doc in dtls_adapter.hpp) — reset it so the client's next ClientHello is
    /// accepted instead of alert-10'd.
    constexpr std::uint64_t kHalfOpenTimeoutMs = 30000;   // 30 s
    constexpr std::uint64_t kReapIntervalMs    = 5000;    // sweep at most every 5 s
}

log_t dtls_level_from_string(const std::string& s) {
    std::string up;
    up.reserve(s.size());
    for (char c : s) up.push_back(static_cast<char>(std::toupper(
        static_cast<unsigned char>(c))));
    if (up == "DEBUG")   return DTLS_LOG_DEBUG;   // 6 — everything
    if (up == "INFO")    return DTLS_LOG_INFO;    // 5 — all but debug
    if (up == "WARNING") return DTLS_LOG_WARN;    // 3
    if (up == "ERROR")   return DTLS_LOG_CRIT;    // 2 — emerg/alert/crit
    return DTLS_LOG_WARN;                         // quiet, production-safe default
}

void dtls_apply_log_level(data_store::Client& ds) {
    // Per-daemon log.level.dtls wins; fall back to the global log.level.
    std::vector<data_store::Client::GetResult> g;
    std::string lvl;
    if (ds.get({std::string("log.level.dtls"), std::string("log.level")}, g).ok) {
        for (const auto& r : g) {
            if (r.has_value) {
                if (auto v = data_store::to_string(r.value)) {
                    if (!v->empty()) { lvl = *v; break; }
                }
            }
        }
    }
    dtls_set_log_level(dtls_level_from_string(lvl));
}


std::int32_t dtlsWriteCb(dtls_context_t *ctx, session_t *session, uint8 *data, size_t len) {
    DTLSAdapter &inst = *static_cast<DTLSAdapter *>(dtls_get_app_data(ctx));
    std::int32_t fd = inst.getFd();
    dtls_debug("dtlsWriteCb --> Sending message to peer length: %d\n", len);
    return sendto(fd, data, len, MSG_DONTWAIT, &session->addr.sa, session->size);
}

std::int32_t dtlsReadCb(dtls_context_t *ctx, session_t *session, uint8 *data, size_t len) {
    std::int32_t ret = -1;

    if(nullptr != session && nullptr != data && len > 0) {
        dtls_debug("dtlsReadCb --> Received deciphered message of length: %d\n", len);
        DTLSAdapter &inst = *static_cast<DTLSAdapter *>(dtls_get_app_data(ctx));
        inst.session(*session);
        std::string deciphered(reinterpret_cast<const char*>(data), len);
        std::vector<std::string> out;
        auto rsp = inst.coapAdapter()->processRequest(inst.isClient(), session, deciphered, out);
        inst.responses(out);
        return(out.size());
    }

    return(ret);
}

std::int32_t dtlsEventCb(dtls_context_t *ctx, session_t *session, dtls_alert_level_t level, unsigned short code) {

    DTLSAdapter &inst = *static_cast<DTLSAdapter *>(dtls_get_app_data(ctx));
    dtls_debug(" Fd: %d\n", inst.getFd());

    if(!level && code > 0xFF) {
        /// This is an internal events.
        dtls_debug("DTLS Internal Events code: %d\n", code);
        switch(code) {
            case DTLS_EVENT_CONNECTED:
            {
                dtls_info("Peer is connected\n");
                // Handshake completed — this endpoint is no longer half-open, so
                // the server-role reaper must stop tracking it.
                if(!inst.isClient() && session) inst.note_peer_connected(*session);
                if(inst.isClient()) {
                    inst.clientState("connected");
                } else {
                    /// @brief For dtls server, only connected event is fired and for client, first connect and then connected events are fired.
                    auto IP = session->addr.sin.sin_addr.s_addr;
                    auto PORT = ntohs(session->addr.sin.sin_port);
                    struct in_addr pp;
                    pp.s_addr = IP;
                    std::string IPStr = inet_ntoa(pp);
                    
                    auto it = std::find_if(inst.clients().begin(), inst.clients().end(), [&](auto& ent) -> bool {
                        return(IPStr == ent.peerIP() && PORT == ent.peerPort());
                    });

                    if(it != inst.clients().end()) {
                        auto& elm = *it;
                        elm.state("connected");
                    } else {
                        DTLSAdapter::ClientDetails client;
                        client.peerIP(IPStr);
                        client.peerPort(PORT);
                        client.state("connected");
                        inst.clients().push_back(client);
                        dtls_debug("PeerIP:%s PeerPort:%d\n", client.peerIP().c_str(), client.peerPort());
                    }
                }
            }
            break;
            case DTLS_EVENT_RENEGOTIATE:
            {
                dtls_info("Peer is renogotiated\n");
            }
            break;
            case DTLS_EVENT_CONNECT:
            {
                dtls_info("Peer is connecting...\n");
                if(inst.isClient()) {
                    inst.clientState("connecting");
                } else {
                    std::uint32_t IP = session->addr.sin.sin_addr.s_addr;
                    struct in_addr pp;
                    pp.s_addr = IP;
                    std::string IPStr = inet_ntoa(pp);

                    auto PORT = ntohs(session->addr.sin.sin_port);
                    
                    auto it = std::find_if(inst.clients().begin(), inst.clients().end(), [&](auto& ent) -> bool {
                        return(IPStr == ent.peerIP() && PORT == ent.peerPort());
                    });

                    if(it != inst.clients().end()) {
                        auto& elm = *it;
                        elm.state("connecting");
                    } else {
                        DTLSAdapter::ClientDetails client;
                        client.peerIP(IPStr);
                        client.peerPort(PORT);
                        client.state("connecting");
                        inst.clients().push_back(client);
                    }
                }
            }
            break;
            default:
                dtls_info("Unknown code: %d\n", code);
                inst.clientState("connect_error");
        }
    } else {
        /// This is an alert message.
        dtls_debug("DTLS Alert Message level: %d  code: %d\n", level, code);
        // Any alert (close_notify or a fatal) tears the peer down — stop tracking
        // it so a fresh handshake from the same endpoint is treated as new.
        if(!inst.isClient() && session) inst.note_peer_gone(*session);
        switch(code) {
            case DTLS_ALERT_CLOSE_NOTIFY:
            {
                dtls_info("Alert close notify\n");
            }
            break;
            default:
                dtls_info("Unhandled Alert: %d\n", code);
        }
    }

    return(0);
}

std::int32_t dtlsGetPskInfoCb(dtls_context_t *ctx, const session_t *session, dtls_credentials_type_t type, const unsigned char *identity, size_t identity_len, unsigned char *result, size_t result_length) {
    (void)session;
    std::string in(reinterpret_cast<const char*>(identity), identity_len);
    DTLSAdapter &inst = *static_cast<DTLSAdapter *>(dtls_get_app_data(ctx));
    // BUG-001 / NFR-SEC-001: do not log identity bytes or the PSK on
    // INFO. The handshake state is sufficient for debugging; DEBUG
    // captures the rest under operator control.
    dtls_debug("dtlsGetPskInfoCb Fd:%d type:%d id_len:%zu result_len:%zu\n",
               inst.getFd(), type, identity_len, result_length);

    switch(type) {
        case DTLS_PSK_HINT:
        {
            // No hint to advertise; return 0 (length) and 0 bytes written.
            return(0);
        }
        case DTLS_PSK_IDENTITY:
        {
            // BUG-001 fix:
            //   * Use match_identity only when the server sent a real hint
            //     (id_len > 0). Empty hint → fall back to our configured
            //     identity directly; do not look up an empty key.
            //   * Buffer-fit check is `identity_len <= result_length` (the
            //     ORIGINAL code had this backwards: `result_length <= iden.length()`
            //     was the *success* branch instead of the *failure* branch).
            std::string iden;
            const bool serverGaveHint = identity_len > 0;
            if (!serverGaveHint || !inst.match_identity(in, iden)) {
                iden = inst.identity();
            }
            if (iden.empty()) {
                dtls_warn("No PSK identity configured\n");
                return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
            }
            if (iden.length() > result_length) {
                dtls_warn("PSK identity (%zu B) exceeds caller buffer (%zu B)\n",
                          iden.length(), result_length);
                return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
            }
            ::memcpy(result, iden.data(), iden.length());
            return static_cast<std::int32_t>(iden.length());
        }
        case DTLS_PSK_KEY:
        {
            // In-memory store first (client's derived identity / explicit
            // creds); then the server's ds-backed resolver, which looks the
            // key up live from cloud.endpoint.credentials by the presented
            // identity — so provisioning takes effect without a restart.
            auto secret = inst.get_secret(in);
            if (secret.empty()) secret = inst.resolve_secret(in);
            if (secret.empty()) {
                dtls_warn("PSK for unknown identity requested\n");
                return dtls_alert_fatal_create(DTLS_ALERT_ILLEGAL_PARAMETER);
            }
            // Secrets are stored as hex strings; convert to binary.
            auto bin = inst.hexToBinary(secret);
            if (bin.empty()) {
                dtls_warn("PSK hex decode failed\n");
                return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
            }
            // BUG-001 fix: correct buffer-fit direction.
            if (bin.size() > result_length) {
                dtls_warn("PSK (%zu B) exceeds caller buffer (%zu B)\n",
                          bin.size(), result_length);
                return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
            }
            // Debug key-dump for offline pcap decryption. The negotiated suite
            // is TLS_PSK_WITH_AES_128_CCM_8 (no ECDHE), so the PSK alone lets
            // Wireshark derive the session keys: paste `secret` into DTLS ->
            // Pre-Shared-Key. Both the BS and DM handshakes pass through here,
            // so a single tap captures every session as the device connects.
            // OFF unless $IOT_DTLS_KEYLOG names a file (SSLKEYLOGFILE-style) —
            // NEVER the journal: the surrounding code deliberately keeps PSKs
            // off the logs (BUG-001 / NFR-SEC-001). The file holds live PSKs;
            // keep it root-only and wipe it after pulling the pcap.
            if (const char* kl = ::getenv("IOT_DTLS_KEYLOG"); kl && *kl) {
                if (FILE* f = ::fopen(kl, "ae")) {   // append, close-on-exec
                    ::fprintf(f, "identity=%s key=%s\n", in.c_str(),
                              secret.c_str());
                    ::fclose(f);
                }
            }
            ::memcpy(result, bin.data(), bin.size());
            return static_cast<std::int32_t>(bin.size());
        }
        default:
            dtls_warn("unsupported request type: %d\n", type);
    }
    /// onError
    return(-1);
}

DTLSAdapter::DTLSAdapter(std::int32_t fd, log_t log_level) {
    dtlsFd = fd;
    dtls_init();
    m_dtls_ctx = dtls_new_context(this);
    // NOTE: do NOT set the tinydtls log level here — it's a PROCESS-GLOBAL and
    // constructing an adapter must not clobber a level already applied from ds.
    // dtls_apply_log_level() (driven by log.level.dtls/log.level) is the sole
    // authority; see udp_adapter.cpp + apps/src/main.cpp.
    (void)log_level;
    dtls_set_handler(m_dtls_ctx, &cb);
    m_coapAdapter = std::make_shared<CoAPAdapter>();
    isClient(false);
    clientState("error");
}

DTLSAdapter::DTLSAdapter() : m_dtls_ctx(nullptr), device_credentials(), dtlsFd(-1) {}

DTLSAdapter::~DTLSAdapter() {
    dtls_free_context(m_dtls_ctx);
    m_dtls_ctx = nullptr;
}

void DTLSAdapter::connect(const std::string& ip, const std::uint16_t& port) {
    session(ip, port);
    // Pin the connect target as the client's outbound peer. m_session is
    // overwritten on every inbound packet (dtlsReadCb), so client-initiated
    // requests (Register/Update) must use this stable copy instead.
    m_peerSession = m_session;
    isClient(true);

    // A previous handshake to this peer may have stalled (dropped
    // HelloVerifyRequest / no ServerHello). tinydtls then keeps the peer in a
    // non-connected state and dtls_connect() only attempts renegotiation
    // (dtls_connect_peer → dtls_renegotiate), which fails — "Error in
    // establishes Channel" — so no fresh ClientHello is ever sent and the
    // client can never recover. Reset such a stale peer first so dtls_connect()
    // starts a clean handshake. A *connected* peer is left untouched so we
    // don't tear down a healthy session.
    dtls_peer_t* existing = dtls_get_peer(dtls_ctx(), &m_session);
    if (existing && dtls_peer_state(existing) != DTLS_STATE_CONNECTED) {
        dtls_debug("DTLSAdapter::connect resetting stale peer for a fresh handshake\n");
        dtls_reset_peer(dtls_ctx(), existing);
    }

    auto ret = dtls_connect(dtls_ctx(), &m_session);
    if(!ret) {
        /// Channel exists
        dtls_debug("DTLSAdapter::connect Channel is already exists\n");
    } else if(ret > 0) {
        /// Establishes new Channel
        dtls_debug("DTLSAdapter::connect Establises new channel for Client Hello\n");
    } else {
        /// Error establishing the channel. The targeted reset above missed a
        /// stuck peer whose session key drifted, so the peer table is full and
        /// dtls_connect() returned "cannot add peer" (a forever wedge — observed
        /// on HW: a device looping "cannot add peer" until a manual restart).
        /// Recreate the context (empty peer table) and retry once.
        dtls_debug("DTLSAdapter::connect dtls_connect error (%d) — hard-resetting context\n", ret);
        hard_reset();
        if (dtls_connect(dtls_ctx(), &m_session) > 0)
            dtls_debug("DTLSAdapter::connect fresh context: new channel for Client Hello\n");
    }
}

void DTLSAdapter::hard_reset() {
    // In-process equivalent of restarting iot-lwm2m-client: free the context
    // (drops ALL peers, busting a stuck/drifted one that targeted resets miss)
    // and rebuild it exactly as the ctor does. The fd, credentials and handler
    // table (cb, keyed off `this`) persist, so the next dtls_connect() opens a
    // clean handshake. Safe here because we only reach this on a connect error
    // (BS/DM DTLS already not up), so no healthy session is being torn down.
    if (m_dtls_ctx) dtls_free_context(m_dtls_ctx);
    m_dtls_ctx = dtls_new_context(this);
    dtls_set_handler(m_dtls_ctx, &cb);
    clientState("");
    dtls_debug("DTLSAdapter::hard_reset recreated dtls context (peer table cleared)\n");
}

void DTLSAdapter::reset_and_connect(const std::string& ip, const std::uint16_t& port,
                                   bool toBootstrapIdentity) {
    // For a BS re-handshake, restore the BS identity so this fresh handshake
    // presents BS creds, not the DM identity+key the BS server cannot resolve.
    // For the DM switch (toBootstrapIdentity=false) the caller has just pinned
    // the DM identity via active_identity(); resetting it here would make the DM
    // handshake present the BS identity (sha256(serial)), which the DM server
    // has no PSK for — the device would wedge at dm-connecting forever.
    if (toBootstrapIdentity) reset_to_bootstrap_identity();
    session(ip, port);
    m_peerSession = m_session;
    isClient(true);
    // Unlike connect(), reset the peer UNCONDITIONALLY — even DTLS_STATE_CONNECTED.
    // The caller knows the session is dead (e.g. a bootstrap POST /bs that never
    // got a reply: tinydtls still reports the peer "connected", so dtls_connect()
    // would only renegotiate — emitting nothing — and the client loops forever
    // POSTing into the void). A clean reset forces a fresh ClientHello.
    dtls_peer_t* existing = dtls_get_peer(dtls_ctx(), &m_session);
    if (existing) {
        dtls_debug("DTLSAdapter::reset_and_connect force-resetting peer\n");
        dtls_reset_peer(dtls_ctx(), existing);
    }
    clientState("");                       // drop the stale app-level state too
    auto ret = dtls_connect(dtls_ctx(), &m_session);
    if (ret > 0) {
        dtls_debug("DTLSAdapter::reset_and_connect new channel for Client Hello\n");
    } else if (ret < 0) {
        // Same wedge guard as connect(): a drifted stuck peer the targeted reset
        // didn't match leaves the table full ("cannot add peer"). Rebuild clean.
        dtls_debug("DTLSAdapter::reset_and_connect error (%d) — hard-resetting context\n", ret);
        hard_reset();
        if (dtls_connect(dtls_ctx(), &m_session) > 0)
            dtls_debug("DTLSAdapter::reset_and_connect fresh context: new channel\n");
    }
}

void DTLSAdapter::note_peer_activity(const session_t& s) {
    if (isClient()) return;                      // server-role safeguard only
    const std::string key = dtls_session_key(s);
    // Insert-if-absent: keep the ORIGINAL first-seen so age accrues across the
    // client's retransmits. A CONNECTED peer that later re-appears here is
    // harmless — reap_stale_peers() re-checks the live state before resetting.
    if (m_halfOpen.find(key) == m_halfOpen.end())
        m_halfOpen.emplace(key, HalfOpenPeer{ dtls_now_ms(), s });
}

void DTLSAdapter::note_peer_connected(const session_t& s) {
    m_halfOpen.erase(dtls_session_key(s));       // handshake completed — not stale
}

void DTLSAdapter::note_peer_gone(const session_t& s) {
    m_halfOpen.erase(dtls_session_key(s));       // peer closed/invalidated — untrack
}

void DTLSAdapter::reap_stale_peers() {
    if (isClient() || !dtls_ctx()) return;
    const std::uint64_t now = dtls_now_ms();
    if (now - m_lastReapMs < kReapIntervalMs) return;   // throttle the sweep
    m_lastReapMs = now;

    for (auto it = m_halfOpen.begin(); it != m_halfOpen.end(); ) {
        if (now - it->second.sinceMs < kHalfOpenTimeoutMs) { ++it; continue; }
        // Past the timeout. Only reset if a peer is genuinely still lingering in
        // a non-CONNECTED state — the live state check protects a healthy peer
        // that merely re-appeared in the map after connecting.
        dtls_peer_t* peer = dtls_get_peer(dtls_ctx(), &it->second.session);
        if (peer && dtls_peer_state(peer) != DTLS_STATE_CONNECTED) {
            ACE_ERROR((LM_WARNING,
                ACE_TEXT("%D lwm2m:thread:%t %M %N:%l reaping stale half-open DTLS "
                         "peer %C (state=%d, %ums old) — freeing it so the next "
                         "ClientHello starts a fresh handshake\n"),
                it->first.c_str(),
                static_cast<int>(dtls_peer_state(peer)),
                static_cast<unsigned>(now - it->second.sinceMs)));
            dtls_reset_peer(dtls_ctx(), peer);   // dtls_destroy_peer(...,1): unlinks it
        }
        it = m_halfOpen.erase(it);
    }
}

std::int32_t DTLSAdapter::rx(std::int32_t fd) {
    std::int32_t ret = -1;
    std::vector<std::uint8_t> buf(DTLS_MAX_BUF);
    int len;
    dtls_debug("DTLSAdapter::rx on Fd: %d\n", fd);
    session_t session;
    memset(&session, 0, sizeof(session_t));
    session.size = sizeof(m_session.addr);
    len = recvfrom(fd, buf.data(), buf.size(), MSG_TRUNC, &session.addr.sa, &session.size);
    dtls_debug("DTLSAdapter::rx len: %d\n", len);

    if(len < 0) {
        perror("recvfrom");
        return ret;
    } else {
        buf.resize(len);
        dtls_debug("got %d bytes from port %d\n", len, ntohs(session.addr.sin.sin_port));
        ACE_DEBUG((LM_DEBUG,
                   ACE_TEXT("%D lwm2m:thread:%t %M %N:%l rx %d bytes from port %u\n"),
                   len, static_cast<unsigned>(ntohs(session.addr.sin.sin_port))));

        if(len <= DTLS_MAX_BUF) {
            dtls_debug_dump("bytes from peer:", buf.data(), len);
            // Track this endpoint for the server-role half-open reaper BEFORE
            // handling — a wedged handshake keeps arriving here and never fires
            // the CONNECTED event that would untrack it.
            note_peer_activity(session);
            /// @brief This function deciphers the raw data received from peer and
            ///        invokes registered callback (dtlsReadCb) to deliver deciphered message.
            auto ret = dtls_handle_message(dtls_ctx(), &session, (unsigned char *)&buf.at(0), len);
            
            if(!ret) {
                dtls_debug("Message is deciphered successfully\n");
                // Reply to the peer this datagram CAME FROM (the captured
                // `session`), not m_session. Handling the message may have
                // switched m_session to a different peer — e.g. a
                // Bootstrap-Finish triggers the client to connect() to the DM
                // mid-handling — and replying to m_session would then leak the
                // Bootstrap ACK onto the freshly-opened DM session.
                for(auto rsp: responses()) {
                    dtls_debug("Sending response to peer\n");
                    dtls_write(dtls_ctx(), &session,
                               (std::uint8_t *)&rsp.at(0), rsp.size());
                }
            }
            // Opportunistic sweep (server role, self-throttled): reset any peer
            // wedged half-open so this endpoint's next ClientHello is accepted.
            // Runs here because a wedged handshake keeps landing in rx().
            reap_stale_peers();
            return(ret);
        } else {
            dtls_debug_dump("bytes from peer: ", buf.data(), buf.size());
            dtls_warn("%d bytes exceeds buffer %d, drop message! \n", len, DTLS_MAX_BUF);
            return(-1);
        }
    }
    
    return(-1);
}

std::int32_t DTLSAdapter::tx(std::string& in) {
    std::int32_t ret = -1;
    ret = dtls_write(dtls_ctx(), &m_session, (std::uint8_t *)&in.at(0), in.size());
    return(ret);
}

std::int32_t DTLSAdapter::tx_peer(std::string& in) {
    // Client-initiated send to the pinned connect target (m_peerSession),
    // immune to m_session being overwritten by inbound packets from another
    // peer (e.g. lingering Bootstrap retransmits after switching to the DM).
    return dtls_write(dtls_ctx(), &m_peerSession,
                      (std::uint8_t *)&in.at(0), in.size());
}

std::int32_t DTLSAdapter::tx(std::string& in, std::string peerIP, std::uint16_t peerPort) {
    // Build the peer session EXACTLY as session() / the inbound rx path do:
    // zero the whole struct first (so ifindex — which dtls_session_equals
    // compares — is 0, not garbage) and fill addr.sin + size =
    // sizeof(sockaddr_in). Without the memset the stale ifindex makes
    // dtls_write fail to match the established inbound peer and instead kick
    // off a NEW handshake — so a server-initiated send (cert push, liveness
    // poll) never reaches the already-connected device.
    session_t peersession;
    ::memset(&peersession, 0, sizeof(peersession));
    peersession.addr.sin.sin_family      = AF_INET;
    peersession.addr.sin.sin_addr.s_addr = inet_addr(peerIP.c_str());
    peersession.addr.sin.sin_port        = htons(peerPort);
    ::memset(peersession.addr.sin.sin_zero, 0, sizeof(peersession.addr.sin.sin_zero));
    peersession.size = sizeof(struct sockaddr_in);

    return dtls_write(dtls_ctx(), &peersession,
                      (std::uint8_t *)&in.at(0), in.size());
}






#endif /* __dtls_adapter_cp__*/