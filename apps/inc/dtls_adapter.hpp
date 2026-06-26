#ifndef __dtls_adapter_hpp__
#define __dtls_adapter_hpp__

#include <memory>
#include <vector>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <unordered_map>
#include <functional>

#include "coap_adapter.hpp"

/**
 *  @brief Let the c++ compiler know not to mangle the c's functions name because this is an external functions of c not c++,
 *         If we don't do this then c++ compiler mangled the c's functions name and this will fail at linking with tinydtls static library
 *         becasue function names are mangled/changed.
 *
 */
extern "C"
{
    #include <netdb.h>     // getaddrinfo for DTLSAdapter::session(host, port)
    #include "glob.h"
    #include "tinydtls.h"
    #include "dtls_debug.h"
    #include "dtls.h"
    #include "session.h"
    #include "alert.h"

    /**
     * @brief This Function is invoked by tinydtls to send message to peer. The message/data will be encrypted with PSK Key.
     * @param This is a context of tinydtls which has app pointer data member which points to the instance of DTLSAdapter.
     * @param This hold the peer's connection information like peer ID and peer Port
     * @param data to be sent
     * @param length of data to be sent
     * @return number of bytes sent to peer or -1 on error/failure
    */
    std::int32_t dtlsWriteCb(dtls_context_t *ctx, session_t *session, uint8 *data, size_t len);
    /**
     * @brief This Function is invoked by tinydtls to receive/get message from peer. The message/data will be encrypted with PSK Key.
     * @param This is a context of tinydtls which has app pointer data member which points to the instance of DTLSAdapter.
     * @param This hold the peer's connection information like peer ID and peer Port
     * @param data received from peer
     * @param length of data buffer that can be put into.
     * @return number of bytes received from peer or -1 on error/failure
    */
    std::int32_t dtlsReadCb(dtls_context_t *ctx, session_t *session, uint8 *data, size_t len);
    /**
     * @brief This Function is invoked by tinydtls to deliver the events to application of tinydtls.
     * @param This is a context of tinydtls which has app pointer data member which points to the instance of DTLSAdapter.
     * @param This hold the peer's connection information like peer ID and peer Port
     * @param level of events
     * @param code the numerical value of events.
     * @return return 0
    */
    std::int32_t dtlsEventCb(dtls_context_t *ctx, session_t *session, dtls_alert_level_t level, unsigned short code);
    /**
     * @brief This Function is invoked by tinydtls when it received first encripted message from peer. It invokes this callback to get secret for a given identity
     *        The identity is received in DTLS handshake message.Identity is also think of as public key.
     * @param This is a context of tinydtls which has app pointer data member which points to the instance of DTLSAdapter.
     * @param This hold the peer's connection information like peer ID and peer Port.
     * @param type for which this callback was invoked.See the switch case for the type it is invoked.
     * @param identity for which PSK Key to be retrieved.
     * @param identity_len this is the length of identity.
     * @param result will hold the PSK key that will be used by tinydtls for ciphering of messages.
     * @param result_length is the length of PSK is set by tinydtls while invoking it.
     * @return returns the length of PSK key or -1 on Error.
    */
    std::int32_t dtlsGetPskInfoCb(dtls_context_t *ctx, const session_t *session, dtls_credentials_type_t type, const unsigned char *identity, size_t identity_len, unsigned char *result, size_t result_length);
}

// C++ linkage (NOT in the extern "C" block above — these take C++ types).
namespace data_store { class Client; }

/// Map an iot log-level string ("DEBUG"/"INFO"/"WARNING"/"ERROR", any case) to
/// the nearest tinydtls log_t. tinydtls has no ERROR level — ERROR maps to
/// DTLS_LOG_CRIT (emerg/alert/crit only). Unknown/empty → DTLS_LOG_WARN.
log_t dtls_level_from_string(const std::string& s);

/// Resolve log.level.dtls (falling back to log.level) from ds and push it into
/// tinydtls' GLOBAL logger via dtls_set_log_level. Call at startup (after the
/// DTLS contexts exist) and on every log-level watch event — tinydtls logging
/// is otherwise frozen at the value set when the first DTLSAdapter was
/// constructed, ignoring the configured level entirely.
void dtls_apply_log_level(data_store::Client& ds);

class DTLSAdapter {
    public:

        struct ClientDetails {
            ClientDetails(): m_peerIP(""), m_peerPort(0), m_ep(""), m_lt(0), m_ts(0) {
            }

            ~ClientDetails() = default;

            std::string m_peerIP;
            std::uint16_t m_peerPort;
            std::string m_ep;
            std::uint32_t m_lt;
            std::uint64_t m_ts;
            std::string m_state;

            void peerIP(std::string ip) {
                m_peerIP = ip;
            }
            std::string peerIP() {
                return(m_peerIP);
            }

            void peerPort(std::uint16_t port) {
                m_peerPort = port;
            }
            std::uint16_t peerPort() {
                return(m_peerPort);
            }

            void ep(std::string endpoint) {
                m_ep = endpoint;
            }
            std::string ep() {
                return(m_ep);
            }

            void lt(std::uint32_t lifetime) {
                m_lt  = lifetime;
            }
            std::uint32_t lt() {
                return(m_lt);
            }

            void ts(std::uint64_t timestamp) {
                m_ts = timestamp;
            }
            std::uint64_t ts() {
                return(m_ts);
            }

            void state(std::string st) {
                m_state = st;
            }
            std::string state() {
                return(m_state);
            }

        };

        dtls_handler_t cb = {
            /// send encrypted data to peer, This callback will be invoked by dtls when dtls_write is invoked by application.
            .write = dtlsWriteCb,
            /// read decrypted data from peer, This callback is invoked by dtla when dtls_handle_message is invoked by application
            .read  = dtlsReadCb, 
            .event = dtlsEventCb,
            .get_psk_info = dtlsGetPskInfoCb,
            .get_ecdsa_key = nullptr,
            .verify_ecdsa_key = nullptr
        };
    
        DTLSAdapter(std::int32_t fd, log_t log_level);
        DTLSAdapter();
        ~DTLSAdapter();

        std::int32_t rx(std::int32_t fd);
        std::int32_t tx(std::string& in);
        std::int32_t tx(std::string& in, std::string peerIP, std::uint16_t peerPort);
        /// Send to the dedicated client-outbound peer session (the last
        /// connect() target), NOT m_session — which dtlsReadCb overwrites
        /// with whichever peer last sent us a packet. A client that talks to
        /// two servers (Bootstrap, then DM) must keep sending Register/Update
        /// to the DM even while stray Bootstrap retransmits arrive, so its
        /// own requests use this fixed peer session.
        std::int32_t tx_peer(std::string& in);
        void connect(const std::string& ip, const std::uint16_t& port);
        /// Like connect(), but force-tears-down even a *CONNECTED* peer first so
        /// a known-suspect session (e.g. a bootstrap that never completed over a
        /// stale "connected" peer) gets a fresh ClientHello instead of a doomed
        /// renegotiation. Use when the session must be assumed dead.
        void reset_and_connect(const std::string& ip, const std::uint16_t& port);

        /**
         * @brief 
         * 
         * @param identity 
         * @param secret 
         */
        void add_credential(const std::string& identity, const std::string& secret) {
            // Upsert, NOT insert: a re-bootstrap must be able to refresh the DM
            // PSK for an identity already present (the cloud may have rotated /
            // re-provisioned it). std::*map::insert keeps the OLD secret on a
            // duplicate key, which wedged the DM DTLS handshake with a stale
            // credential after a re-bootstrap ("identity already registered" →
            // dtls: cannot send ClientHello → never registers → cloud shows the
            // device offline even with the VPN up). insert_or_assign replaces it.
            auto res = device_credentials.insert_or_assign(identity, secret);
            if(!res.second) {
                ACE_DEBUG((LM_DEBUG,
                           ACE_TEXT("%D lwm2m:thread:%t %M %N:%l add_credential: "
                                    "refreshed secret for existing identity\n")));
            }
        }

        /**
         * @brief Get the secret object
         * 
         * @param identity 
         * @return std::string 
         */
        std::string get_secret(const std::string& identity) {
            auto it = std::find_if(device_credentials.begin(), device_credentials.end(), [&](const auto& ent) -> bool {return(identity == ent.first);});
            if(it != device_credentials.end()) {
                return(it->second);
            }
            return(std::string());
        }

        /// Server-role PSK resolver: looks the secret up live from the data
        /// store for the identity the client presented during the handshake,
        /// so newly-provisioned endpoints authenticate without a restart and
        /// ds stays the single source of truth (no in-memory pre-load/watch).
        /// Runs on the handshake/reactor thread — NOT the ds listener thread —
        /// so a blocking ds get() here is safe. Returns "" when unset/unknown.
        using PskResolver = std::function<std::string(const std::string& identity)>;
        void set_psk_resolver(PskResolver r) { m_psk_resolver = std::move(r); }
        std::string resolve_secret(const std::string& identity) {
            return m_psk_resolver ? m_psk_resolver(identity) : std::string();
        }

        /**
         * @brief 
         * 
         * @param iden 
         * @param identity 
         * @return true 
         * @return false 
         */
        bool match_identity(const std::string& iden, std::string& identity) {
            identity.clear();
            auto it = std::find_if(device_credentials.begin(), device_credentials.end(), [&](const auto& ent) -> bool {return(iden == ent.first);});
            
            if(it != device_credentials.end()) {
                identity.assign(it->first);
                return(true);
            }

            return(false);
        }

        std::int32_t getFd() const {
            return(dtlsFd);
        }

        /// Patch in the UDP socket descriptor after the bound socket is
        /// known. Used by ServiceContext_t::open() so tinydtls' send
        /// callback can ::sendto on the right fd. Safe to call from the
        /// reactor thread only.
        void setFd(std::int32_t fd) {
            dtlsFd = fd;
        }

        std::string hexToBinary(const std::string &hex) {
            std::string binary;
            for (size_t i = 0; i < hex.size()/2; ++i) {
                std::stringstream hexToDecimal;
                hexToDecimal << std::hex << hex.substr(2*i, 2);
                int decimal = 0;
                hexToDecimal >> decimal;
                binary.append(1, static_cast<unsigned char>(decimal));
            }
            return binary;
        }
        
        std::shared_ptr<CoAPAdapter>& coapAdapter() {
            return(m_coapAdapter);
        }
        
        
        dtls_context_t* dtls_ctx() {
            return(m_dtls_ctx);
        }

        /// Pump tinydtls' handshake retransmission timer. Without periodic
        /// calls, an unanswered handshake flight (e.g. the bootstrap
        /// ClientHello) is never retransmitted and the connection wedges.
        /// Driven from the client's 1 Hz tick. Returns true if tinydtls has
        /// exhausted its max retransmits for some peer (handshake gave up) —
        /// the caller may then re-initiate via connect().
        bool check_retransmit() {
            if(!m_dtls_ctx) return(false);
            clock_time_t next = 0;
            bool isMaxRetransmit = false;
            dtls_check_retransmit(m_dtls_ctx, &next, &isMaxRetransmit);
            return(isMaxRetransmit);
        }

        std::string& data() {
            return(m_data);
        }

        void data(std::string in) {
            m_data = in;
        }

        void responses(std::vector<std::string> rsps) {
            m_responses = rsps;
        }

        std::vector<std::string> responses() {
            return(m_responses);
        }

        void session(session_t sess) {
            m_session = sess;
        }

        session_t session() {
            return(m_session);
        }

        void session(const std::string& ip, const std::uint16_t& port) {
            ::memset((void *)&m_session, 0, sizeof(m_session));
            // L9 / DTLS interop: inet_addr() returns INADDR_NONE for any
            // non-dotted-decimal input, which the next assignment treats
            // as 255.255.255.255 (broadcast) and tinydtls then fails to
            // send to. Resolve hostnames via getaddrinfo first; fall
            // back to inet_addr only when getaddrinfo fails so existing
            // numeric-IP callers stay correct.
            struct addrinfo hints;
            ::memset(&hints, 0, sizeof(hints));
            hints.ai_family   = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;
            struct addrinfo* result = nullptr;
            int gai = ::getaddrinfo(ip.c_str(), nullptr, &hints, &result);
            if (gai == 0 && result) {
                auto* sin = reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
                m_session.addr.sin.sin_addr = sin->sin_addr;
                ::freeaddrinfo(result);
            } else {
                m_session.addr.sin.sin_addr.s_addr = inet_addr(ip.c_str());
            }
            m_session.addr.sin.sin_family = AF_INET;
            m_session.addr.sin.sin_port = htons(port);
            ::memset(m_session.addr.sin.sin_zero, 0, sizeof(m_session.addr.sin.sin_zero));
            m_session.size = sizeof(struct sockaddr_in);
        }

        void isClient(bool isClient) {
            m_isClient = isClient;
        }
        bool isClient() {
            return(m_isClient);
        }

        void  clientState(std::string st) {
            m_clientState = st;
        }
        std::string clientState() {
            return(m_clientState);
        }

        std::string identity() {
            // Once more than one credential is installed (e.g. the BS
            // identity at startup + the DM identity after bootstrap), the
            // map order is non-deterministic, so the active identity pins
            // which one the client presents for the current peer. Empty =
            // fall back to the first (single-credential) entry.
            if(!m_activeIdentity.empty()) {
                return(m_activeIdentity);
            }
            if(isClient() && !device_credentials.empty()) {
                for(auto ent: device_credentials) {
                    return(ent.first);
                }
            }
            return(std::string());
        }

        /// Pin the PSK identity the client presents on the next handshake.
        /// Used to switch from the BS identity to the bootstrap-delivered DM
        /// identity before reconnecting to the DM server.
        void active_identity(const std::string& id) {
            m_activeIdentity = id;
        }

        auto& clients() {
            return(m_clients);
        }

    private:
        //std::unique_ptr<dtls_context_t, decltype(&dtls_free_context)> dtls_ctx;
        dtls_context_t *m_dtls_ctx;
        std::unordered_map<std::string, std::string> device_credentials;
        PskResolver m_psk_resolver;
        std::int32_t dtlsFd;
        std::shared_ptr<CoAPAdapter> m_coapAdapter;
        session_t m_session;
        session_t m_peerSession{};   ///< fixed client-outbound peer (connect target)
        std::string m_data;
        std::vector<std::string> m_responses;
        bool m_isClient;
        std::string m_clientState;
        std::string m_activeIdentity;
        std::vector<ClientDetails> m_clients;
};






#endif /* __dtls_adapter_hpp__*/