#ifndef __dtls_adapter_cpp__
#define __dtls_adapter_cpp__

#include "dtls_adapter.hpp"


std::int32_t dtlsWriteCb(dtls_context_t *ctx, session_t *session, uint8 *data, size_t len) {
    DTLSAdapter &inst = *static_cast<DTLSAdapter *>(dtls_get_app_data(ctx));
    std::int32_t fd = inst.getFd();
    dtls_debug("dtlsWriteCb --> Sending message to peer length:\n", len);
    return sendto(fd, data, len, MSG_DONTWAIT, &session->addr.sa, session->size);
}

std::int32_t dtlsReadCb(dtls_context_t *ctx, session_t *session, uint8 *data, size_t len) {
    std::int32_t ret = -1;

    if(nullptr != session && nullptr != data && len > 0) {
        DTLSAdapter &inst = *static_cast<DTLSAdapter *>(dtls_get_app_data(ctx));
        inst.session(*session);
        std::string deciphered(reinterpret_cast<const char*>(data), len);
        std::vector<std::string> out;
        auto rsp = inst.coapAdapter()->processRequest(session, deciphered, out);
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
                dtls_info("Peer is connect\n");
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
    dtls_info("In Fn dtlsGetPskInfoCb Fd: %d identity_length: %d result_length: %d\n", inst.getFd(), identity_len, result_length);

    switch(type) {
        case DTLS_PSK_HINT:
        {
            return(0);
        }
        case DTLS_PSK_IDENTITY:
        {
            std::string iden;
            if(inst.match_identity(in, iden) && result_length <= iden.length()) {
                ::memcpy(result, iden.data(), iden.length());
                return(iden.length());
            } else {
                iden = inst.identity();
                ::memcpy(result, iden.data(), iden.length());
                dtls_debug("The identity length:%d value:%s\n", iden.length(), iden.c_str());
                return(iden.length());
            }
        }
        case DTLS_PSK_KEY:
        {
            dtls_debug("For PSK The identity length:%d value:%s\n", identity_len, identity);
            auto secret = inst.get_secret(in);
            if(secret.empty()) {
                dtls_warn("Can't retrieve PSK for an empty identity\n");
                return dtls_alert_fatal_create(DTLS_ALERT_ILLEGAL_PARAMETER);
            }

            auto secret128Bits = inst.hexToBinary(secret);
            if(secret128Bits.size() > 0 && result_length <= secret128Bits.size()) {
                ::memcpy(result, secret128Bits.data(), secret128Bits.size());
                return secret128Bits.size();
            } else if(result_length < secret128Bits.size()) {
                dtls_warn("can't set psk -- buffer too small Underflow\n");
                return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
            } else {
                ::memcpy(result, secret.data(), secret.size());
                return(secret.length());
                //dtls_warn("PSK for unknown id requested, exiting\n");
                //return dtls_alert_fatal_create(DTLS_ALERT_ILLEGAL_PARAMETER);
            }
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
    dtls_set_log_level(log_level);
    dtls_set_handler(m_dtls_ctx, &cb);
    m_coapAdapter = std::make_shared<CoAPAdapter>();
    isClient(false);
    clientState("error");
}

DTLSAdapter::DTLSAdapter() : m_dtls_ctx(nullptr), device_credentials() {}

DTLSAdapter::~DTLSAdapter() {
    dtls_free_context(m_dtls_ctx);
    m_dtls_ctx = nullptr;
}

void DTLSAdapter::connect(const std::string& ip, const std::uint16_t& port) {
    session(ip, port);
    isClient(true);

    auto ret = dtls_connect(dtls_ctx(), &m_session);
    if(!ret) {
        /// Channel exists
        dtls_debug("DTLSAdapter::connect Channel is already exists\n");
    } else if(ret > 0) {
        /// Establishes new Channel
        dtls_debug("DTLSAdapter::connect Establises new channel for Client Hello\n");
    } else {
        /// Error in establishes channel
        dtls_debug("DTLSAdapter::connect Error in establishes Channel\n");
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
        std::cout << basename(__FILE__) << ":" << __LINE__ << " got len: " << len << " bytes from port: " << 
            ntohs(session.addr.sin.sin_port) << std::endl;

        if(len <= DTLS_MAX_BUF) {
            dtls_debug_dump("bytes from peer:", buf.data(), len);
            /// This function deciphers the raw data received from peer and invokes registered callback to deliver decipher message.
            auto ret = dtls_handle_message(dtls_ctx(), &session, (unsigned char *)&buf.at(0), len);
            //dtls_debug("Message is deciphered successfully\n");
            if(ret > 0) {
                for(auto rsp: responses()) {
                    tx(rsp);
                }
            }
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

std::int32_t DTLSAdapter::tx(std::string& in, std::string peerIP, std::uint16_t peerPort) {
    std::int32_t ret = -1;
    struct sockaddr_in addr;

    addr.sin_addr.s_addr = inet_addr(peerIP.c_str());
    addr.sin_family = AF_INET;
    addr.sin_port = htons(peerPort);
    ::memset(addr.sin_zero, 0, sizeof(addr.sin_zero));

    session_t peersession;
    peersession.addr.sa = *((struct  sockaddr *)&addr);
    peersession.size = sizeof(peersession.addr.sa);
    
    ret = dtls_write(dtls_ctx(), &peersession, (std::uint8_t *)&in.at(0), in.size());
    return(ret);
}






#endif /* __dtls_adapter_cp__*/