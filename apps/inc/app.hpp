#ifndef __app_hpp__
#define __app_hpp__

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
#include "lwm2m_adapter.hpp"
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


class App {
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
            std::int32_t fd;
            std::string peerHost;
            std::uint16_t peerPort;
            std::string selfHost;
            std::uint16_t selfPort;
            Scheme_t scheme;
            ServiceType_t service;
            std::unique_ptr<LwM2MAdapter> m_lwm2mAdapter;
            std::unique_ptr<CoAPAdapter> m_coapAdapter;
            std::unique_ptr<DTLSAdapter> m_dtlsAdapter;

            ServiceContext_t(std::int32_t Fd, Scheme_t schm) {
                if(schm == App::Scheme_t::CoAPs) {
                    //DTLS_LOG_INFO
                    m_dtlsAdapter = std::make_unique<DTLSAdapter>(Fd, DTLS_LOG_DEBUG, this);
                }
                m_coapAdapter = std::make_unique<CoAPAdapter>();
                m_lwm2mAdapter = std::make_unique<LwM2MAdapter>();

                fd = Fd;
                scheme = schm;
            }

             ServiceContext_t() = delete;
            ~ServiceContext_t() {
                std::cout << basename(__FILE__) << ":" << __LINE__ << " Closing Socket:" << fd << std::endl;
                ::close(fd);
            }

            void set_peerHost(std::string host) {
                peerHost = host;
            }
            std::string& get_peerHost() {
                return(peerHost);
            }

            void set_peerPort(std::uint16_t port) {
                peerPort = port;
            }
            std::uint16_t& get_peerPort() {
                return(peerPort);
            }

            void set_selfHost(std::string host) {
                selfHost = host;
            }
            std::string& get_selfHost() {
                return(selfHost);
            }

            void set_selfPort(std::uint16_t port) {
                selfPort = port;
            }
            std::uint16_t& get_selfPort() {
                return(selfPort);
            }

            void set_scheme(Scheme_t sc) {
                scheme = sc;
            }
            Scheme_t& get_scheme() {
                return(scheme);
            }

            void set_service(ServiceType_t sc) {
                service = sc;
            }
            ServiceType_t& get_service() {
                return(service);
            }

            std::int32_t get_fd() {
                return(fd);
            }

            DTLSAdapter& get_dtls_adapter() {
                return(*m_dtlsAdapter.get());
            }

            CoAPAdapter& get_coap_adapter() {
                return(*m_coapAdapter.get());
            }

            LwM2MAdapter& get_lwm2m_adapter() {
                return(*m_lwm2mAdapter.get());
            }

        };

    public:

        App(std::string& host, std::uint16_t& port, Scheme_t& scheme, ServiceType_t& service) {
            if(!init(host, port, scheme, service)) {
                epollFd = ::epoll_create1(EPOLL_CLOEXEC);
            }
        }

        ~App() {
            
            ::close(epollFd);
        }

        App& operator=(App& rhs) = default;
        App& operator=(const App& rhs) = default;
        App(const App& rhs) = default;
        
        std::int32_t add_event_handle(const Scheme_t& scheme, const ServiceType_t& svc);
        std::int32_t init(const std::string& host, const std::uint16_t& port, const Scheme_t& scheme);
        std::int32_t init(const std::string& host, const std::uint16_t& port, const Scheme_t& scheme, const ServiceType_t& service);
        std::int32_t start(Role_t role, Scheme_t scheme);
        std::int32_t stop();
        std::int32_t rx(std::int32_t fd);
        std::int32_t rx(std::int32_t fd, std::string& out, std::uint32_t& peerIP, std::uint16_t& peerPort);
        std::int32_t tx(std::string& in, ServiceType_t& service);

        void hex_dump(const std::string& in);
        std::int32_t handle_io_coaps(const std::int32_t& handle, const ServiceType_t& service);
        std::int32_t handle_io_coap(const std::int32_t& handle, const ServiceType_t& service);
        std::int32_t handle_io(const std::int32_t& fd, const Scheme_t& scheme, const ServiceType_t&  serverType);

        std::unordered_map<App::ServiceType_t, std::unique_ptr<App::ServiceContext_t>>&  get_services() {
            return(services);
        }

    private:
        std::int32_t epollFd;
        std::vector<struct epoll_event> evts;
        std::unordered_map<App::ServiceType_t, std::unique_ptr<App::ServiceContext_t>> services;
        
};

#endif /*__app_hpp__*/