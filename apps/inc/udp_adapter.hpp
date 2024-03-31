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
#include "lwm2m_adapter.hpp"


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
            BootsstrapServer = 1,
            DeviceMgmtClient = 3,
            LwM2MClient = 4
        } ServiceType_t;

        struct ServiceContext_t  {

            std::int32_t m_fd;
            std::string m_peerHost;
            std::uint16_t m_peerPort;
            std::string m_selfHost;
            std::uint16_t m_selfPort;
            Scheme_t m_scheme;
            ServiceType_t m_service;
            std::unique_ptr<LwM2MAdapter> m_lwm2mAdapter;
            std::unique_ptr<CoAPAdapter> m_coapAdapter;
            std::unique_ptr<DTLSAdapter> m_dtlsAdapter;

            ServiceContext_t(std::int32_t Fd, Scheme_t scheme) {

                if(scheme == UDPAdapter::Scheme_t::CoAPs) {
                    m_dtlsAdapter = std::make_unique<DTLSAdapter>(Fd, DTLS_LOG_DEBUG);
                }

                m_coapAdapter = std::make_unique<CoAPAdapter>();
                m_lwm2mAdapter = std::make_unique<LwM2MAdapter>();

                m_fd = Fd;
                m_scheme = scheme;
            }

             ServiceContext_t() = delete;
            ~ServiceContext_t() {
                std::cout << basename(__FILE__) << ":" << __LINE__ << " Closing Socket:" << m_fd << std::endl;
                ::close(m_fd);
            }

            void peerHost(std::string host) {
                m_peerHost = host;
            }
            std::string& peerHost() {
                return(m_peerHost);
            }

            void peerPort(std::uint16_t port) {
                m_peerPort = port;
            }
            std::uint16_t& peerPort() {
                return(m_peerPort);
            }

            void selfHost(std::string host) {
                m_selfHost = host;
            }
            std::string& selfHost() {
                return(m_selfHost);
            }

            void selfPort(std::uint16_t port) {
                m_selfPort = port;
            }
            std::uint16_t& selfPort() {
                return(m_selfPort);
            }

            void scheme(Scheme_t sc) {
                m_scheme = sc;
            }
            Scheme_t& scheme() {
                return(m_scheme);
            }

            void service(ServiceType_t sc) {
                m_service = sc;
            }
            ServiceType_t& service() {
                return(m_service);
            }

            std::int32_t fd() {
                return(m_fd);
            }

            DTLSAdapter& dtlsAdapter() {
                return(*m_dtlsAdapter.get());
            }

            CoAPAdapter& coapAdapter() {
                return(*m_coapAdapter.get());
            }

            LwM2MAdapter& lwm2mAdapter() {
                return(*m_lwm2mAdapter.get());
            }
        };

    public:

        UDPAdapter(std::string& host, std::uint16_t& port, Scheme_t& scheme, ServiceType_t& service) {
            if(!init(host, port, scheme, service)) {
                m_epollFd = ::epoll_create1(EPOLL_CLOEXEC);
            }
        }

        ~UDPAdapter() {
            ::close(m_epollFd);
        }
        
        std::int32_t add_event_handle(const Scheme_t& scheme, const ServiceType_t& svc);
        std::int32_t init(const std::string& host, const std::uint16_t& port, const Scheme_t& scheme);
        std::int32_t init(const std::string& host, const std::uint16_t& port, const Scheme_t& scheme, const ServiceType_t& service);
        std::int32_t start(Role_t role, Scheme_t scheme);
        std::int32_t stop();
        std::int32_t rx(std::int32_t fd);
        std::int32_t rx(std::int32_t fd, std::string& out, std::uint32_t& peerIP, std::uint16_t& peerPort);
        std::int32_t tx(std::string& in, ServiceType_t& service);
        std::int32_t process_request(const std::string& in, const std::unique_ptr<UDPAdapter::ServiceContext_t>& ctx, CoAPAdapter::CoAPMessage& message);

        void hex_dump(const std::string& in);
        std::int32_t handle_io_coaps(const std::int32_t& handle, const ServiceType_t& service);
        std::int32_t handle_io_coap(const std::int32_t& handle, const ServiceType_t& service);
        std::int32_t handle_io(const std::int32_t& fd, const Scheme_t& scheme, const ServiceType_t&  serverType);

        std::unordered_map<UDPAdapter::ServiceType_t, std::unique_ptr<UDPAdapter::ServiceContext_t>>&  services() {
            return(m_services);
        }

    private:
        std::int32_t m_epollFd;
        std::vector<struct epoll_event> m_evts;
        std::unordered_map<UDPAdapter::ServiceType_t, std::unique_ptr<UDPAdapter::ServiceContext_t>> m_services;
        Role_t m_role;
};















#endif /*__udp_adapter_hpp__*/