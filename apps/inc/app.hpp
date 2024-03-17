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

typedef enum {
    CoAPs = 1,
    CoAP = 2
} Scheme_t;

typedef enum {
    SERVER = 1,
    CLIENT = 2
} Role_t;

typedef enum {
    DeviceMgmt  = 0,
    BootsstrapMgmt = 1,
    UNKNOWN
}ServeerType_t;

class App {
    public:

        App(std::string& host, std::uint16_t& port, Scheme_t& scheme) {
            set_selfHost(host);
            set_selfPort(port);
            this->scheme = scheme;

            serverFd = ::socket(AF_INET, SOCK_DGRAM, 0);
            
            if(serverFd < 0) {
            }
            
            struct sockaddr_in selfAddr;
            struct addrinfo *result;

            auto s = getaddrinfo(host.data(), std::to_string(port).c_str(), nullptr, &result);
            if (!s) {
                selfAddr = *((struct sockaddr_in*)(result->ai_addr));
                freeaddrinfo(result);
            }

            socklen_t len = sizeof(selfAddr);
            auto status = ::bind(serverFd, (struct sockaddr *)&selfAddr, len);
            if(status < 0) {
                std::cout << "fn:"<<__PRETTY_FUNCTION__ << ":" << __LINE__ << " bind failed error:"<< std::strerror(errno) << std::endl;
            }
                        
            std::cout << "fn:" << __PRETTY_FUNCTION__ << ":" << __LINE__ << " created handle:" << serverFd << std::endl;

            if(scheme == CoAPs) {
                //DTLS_LOG_INFO
                dtls_adapter = std::make_unique<DTLSAdapter>(serverFd, DTLS_LOG_DEBUG);
            }

            coapAdapter = std::make_unique<CoAPAdapter>();
        }

        ~App() {
            ::close(serverFd);
            ::close(epollFd);
            dtls_adapter.reset(nullptr);
            coapAdapter.reset(nullptr);
        }

        App& operator=(App& rhs) = default;
        App& operator=(const App& rhs) = default;
        App(const App& rhs) = default;
        
        std::int32_t init(const Scheme_t& scheme);
        std::int32_t init(const std::string& host, const std::uint16_t& port, const Scheme_t& scheme);
        std::int32_t start(Role_t role, Scheme_t scheme);
        std::int32_t stop();
        std::int32_t rx(std::int32_t fd);
        std::int32_t tx(std::string& in);
        std::int32_t add_server(const std::int32_t& fd, const Scheme_t& scheme, const ServeerType_t& serverType);

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

        void hex_dump(const std::string& in);
        std::int32_t handle_io_coaps(const std::int32_t& handle);
        std::int32_t handle_io_coap(const std::int32_t& handle);
        DTLSAdapter& get_adapter();
        CoAPAdapter& get_coapAdapter();

    private:
        std::int32_t epollFd;
        std::int32_t serverFd;

        std::vector<struct epoll_event> evts;
        std::unique_ptr<DTLSAdapter> dtls_adapter;
        std::unique_ptr<CoAPAdapter> coapAdapter;

        std::string peerHost;
        std::uint16_t peerPort;
        std::string selfHost;
        std::uint16_t selfPort;
        Scheme_t scheme;
};

#endif /*__app_hpp__*/