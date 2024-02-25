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

class App {
    public:

        App(const std::string& uri) {
            std::uint16_t port = 5684;
            std::string host;
            serverFd = ::socket(AF_INET, SOCK_DGRAM, 0);
            
            if(serverFd < 0) {
            }

            if(uri.compare(0, 5, "coaps")) {
                /// throw an exception
                std::cout << ::basename(__FILE__) << ":" << __LINE__ << " Invalid uri" << uri << std::endl;
            }

            port = 5684;
            auto offset = std::string("coaps://").length();
            host.assign(uri.substr(offset));
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
            //DTLS_LOG_INFO
            dtls_adapter = std::make_unique<DTLSAdapter>(serverFd, DTLS_LOG_DEBUG);
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
        
        std::int32_t init();
        std::int32_t start();
        std::int32_t stop();
        void hex_dump(const std::string& in);
        std::int32_t handle_io(std::int32_t handle);
        DTLSAdapter& get_adapter();
        CoAPAdapter& get_coapAdapter();

    private:
        std::int32_t epollFd;
        std::int32_t serverFd;
        std::vector<struct epoll_event> evts;
        std::unique_ptr<DTLSAdapter> dtls_adapter;
        std::unique_ptr<CoAPAdapter> coapAdapter;
};

#endif /*__app_hpp__*/