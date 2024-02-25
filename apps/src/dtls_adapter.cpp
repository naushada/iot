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
        std::string in(reinterpret_cast<const char*>(data), len);
        auto rsp = inst.get_coapAdapter().processRequest(session, in);
        return(rsp);
    }

    return(ret);
}

std::int32_t dtlsEventCb(dtls_context_t *ctx, session_t *session, dtls_alert_level_t level, unsigned short code) {
    (void)session;
    DTLSAdapter &inst = *static_cast<DTLSAdapter *>(dtls_get_app_data(ctx));
    dtls_debug(" Fd: %d\n", inst.getFd());

    if(!level && code > 0xFF) {
        /// This is an internal events.
        dtls_debug("DTLS Internal Events code: %d\n", code);
        switch(code) {
            case DTLS_EVENT_CONNECTED:
            {
                dtls_info("Peer is connected\n");
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
            }
            break;
            default:
                dtls_info("Unknown code: %d\n", code);
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
                dtls_warn("cannot set psk_identity -- buffer too small\n");
                return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
            }
        }
        case DTLS_PSK_KEY:
        {
            auto secret = inst.get_secret(in);
            auto secret128Bits = inst.hexToBinary(secret);
            if(secret128Bits.size() > 0 && result_length <= secret128Bits.size()) {
                ::memcpy(result, secret128Bits.data(), secret128Bits.size());
                return secret128Bits.size();
            } else if(result_length < secret128Bits.size()) {
                dtls_warn("cannot set psk -- buffer too small Underflow\n");
                return dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR);
            } else {
                dtls_warn("PSK for unknown id requested, exiting\n");
                return dtls_alert_fatal_create(DTLS_ALERT_ILLEGAL_PARAMETER);
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
    dtls_ctx = dtls_new_context(this);
    dtls_set_log_level(log_level);
    dtls_set_handler(dtls_ctx, &cb);
    coapAdapter = std::make_unique<CoAPAdapter>();
}

DTLSAdapter::DTLSAdapter() : dtls_ctx(nullptr), device_credentials() {}

DTLSAdapter::~DTLSAdapter() {
    dtls_free_context(dtls_ctx);
    dtls_ctx = nullptr;
}

std::int32_t DTLSAdapter::rx(std::int32_t fd) {
    std::int32_t ret = -1;
    std::vector<std::uint8_t> buf(DTLS_MAX_BUF);
    int len;
    dtls_debug("DTLSAdapter::rx on Fd: %d\n", fd);
    memset(&session, 0, sizeof(session_t));
    session.size = sizeof(session.addr);
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
            dtls_debug_dump("bytes from peer: \n", buf.data(), len);
            /// This function deciphers the raw data received from peer and invokes registered callback to deliver decipher message.
            auto ret = dtls_handle_message(dtls_ctx, &session, (unsigned char *)&buf.at(0), len);
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
    ret = dtls_write(dtls_ctx, &session, (std::uint8_t *)&in.at(0), in.size());
    return(ret);
}







#endif /* __dtls_adapter_cp__*/