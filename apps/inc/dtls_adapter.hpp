#ifndef __dtls_adapter_hpp__
#define __dtls_adapter_hpp__

#include <memory>
#include <vector>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <unordered_map>

#include "coap_adapter.hpp"

/**
 *  @brief Let the c++ compiler know not to mangle the c's functions name because this is an external functions of c not c++,
 *         If we don't do this then c++ compiler mangled the c's functions name and this will fail at linking with tinydtls static library
 *         becasue function names are mangled/changed.
 *
 */
extern "C"
{
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
        void connect(const std::string& ip, const std::uint16_t& port);

        /**
         * @brief 
         * 
         * @param identity 
         * @param secret 
         */
        void add_credential(const std::string& identity, const std::string& secret) {
            if(!device_credentials.insert(std::pair<std::string, std::string>(identity, secret)).second) {
                std::cout << "add_credential-> identity & secret can't be inserted into STL" << std::endl;
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
            m_session.addr.sin.sin_addr.s_addr = inet_addr(ip.c_str());
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
            if(isClient() && !device_credentials.empty()) {
                for(auto ent: device_credentials) {
                    return(ent.first);
                }
            }
            return(std::string());
        }

        auto& clients() {
            return(m_clients);
        }

    private:
        //std::unique_ptr<dtls_context_t, decltype(&dtls_free_context)> dtls_ctx;
        dtls_context_t *m_dtls_ctx;
        std::unordered_map<std::string, std::string> device_credentials;
        std::int32_t dtlsFd;
        std::shared_ptr<CoAPAdapter> m_coapAdapter;
        session_t m_session;
        std::string m_data;
        std::vector<std::string> m_responses;
        bool m_isClient;
        std::string m_clientState;
        std::vector<ClientDetails> m_clients;
};






#endif /* __dtls_adapter_hpp__*/