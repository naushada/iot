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

#include "dtls_adapter.hpp"
#include "coap_adapter.hpp"

extern "C"
{
    #include <sys/epoll.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <termios.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/epoll.h>
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <sys/un.h>
    #include <netdb.h>
    #include <signal.h>
}

class App;

class UDPAdapter {
    public:
        typedef enum {
            CoAPs = 1,
            CoAP = 2,
            INVALID = 3
        } Scheme_t;

        typedef enum {
            SERVER = 1,
            CLIENT = 2
        } Role_t;

        typedef enum {
            DeviceMgmtServer  = 0,
            BootStrapServer = 1,
            DeviceMgmtClient = 3,
            LwM2MClient = 4
        } ServiceType_t;

        void host(std::string h) {
            m_host = h;
        }
        std::string& host() {
            return(m_host);
        }

        void port(std::uint16_t p) {
            m_port = p;
        }
        std::uint16_t& port() {
            return(m_port);
        }

        void scheme(Scheme_t sc) {
            m_scheme = sc;
        }
        Scheme_t& scheme() {
            return(m_scheme);
        }

        std::int32_t handle() const {
            return(m_fd);
        }
        void handle(std::int32_t fd) {
            m_fd = fd;
        }

        Role_t role() const {
            return(m_role);
        }
        void role(Role_t r) {
            m_role = r;
        }

        std::shared_ptr<DTLSAdapter>& dtlsAdapter() {
            return(m_dtlsAdapter);
        }

    public:
        UDPAdapter(const std::string& host, const std::uint16_t& port, const Role_t& role, Scheme_t& scheme, App* const app) : m_app(app) {

            init(host, port, scheme, role);
            m_dtlsAdapter = nullptr;
 
            if(scheme == UDPAdapter::Scheme_t::CoAPs) {
                m_dtlsAdapter = std::make_shared<DTLSAdapter>(handle(), DTLS_LOG_DEBUG, this);
            }

            m_scheme = scheme;
            
        }

        ~UDPAdapter() {
            ::close(m_fd);
        }
        
        std::int32_t init(const std::string& host, const std::uint16_t& port, const Scheme_t& scheme, const Role_t& role);
        std::int32_t start(Role_t role, Scheme_t scheme);
        std::int32_t stop();
        std::int32_t rx(std::int32_t fd);
        std::int32_t rx(std::int32_t fd, std::string& out, std::uint32_t& peerIP, std::uint16_t& peerPort);
        std::int32_t tx(const std::string& in, const ServiceType_t& service, const std::string& toIP, const std::uint16_t& toPort);
        void hex_dump(const std::string& in);
        

        auto& app() {
            return(*m_app);
        }

        auto& udpAdapter() {
            return(*this);
        }

    private:
        /// @brief could be either Client or Server
        Role_t m_role;
        std::int32_t m_fd;
        /// @brief  local host name or IP
        std::string m_host;
        /// @brief local port to bind with
        std::uint16_t m_port;
        /// @brief could be coap or coaps
        Scheme_t m_scheme;
        std::shared_ptr<DTLSAdapter> m_dtlsAdapter;
        ///@brief back pointer to parent
        App* const m_app;
};















#endif /*__udp_adapter_hpp__*/