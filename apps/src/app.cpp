#ifndef __app_cpp__
#define __app_cpp__

#include "app.hpp"
#include "coap_adapter.hpp"

void App::hex_dump(const std::string& in) {
    std::stringstream oss;
    //oss << std::hex << std::setfill('0');
    for(const auto& ent: in)
    {
        oss << ent << ' ';
    }

    std::string hexDump = oss.str();
    std::cout << hexDump << std::endl;
}

DTLSAdapter& App::get_adapter() {
    return(*dtls_adapter.get());
}

CoAPAdapter& App::get_coapAdapter() {
    return(*coapAdapter.get());
}

std::int32_t App::rx(std::int32_t fd) {
    std::int32_t ret = -1;
    std::vector<std::uint8_t> buf(1400);
    int len;
    struct sockaddr_in session;
    memset(&session, 0, sizeof(session));
    socklen_t slen = sizeof(session);
    len = recvfrom(fd, buf.data(), buf.size(), MSG_TRUNC, (struct sockaddr *)&session, &slen);
    

    if(len < 0) {
        perror("recvfrom");
        return ret;
    } else {
        buf.resize(len);
        std::cout << basename(__FILE__) << ":" << __LINE__ << " got len: " << len << " bytes from port: " << ntohs(session.sin_port) << std::endl;

        if(len <= 1400) {
            CoAPAdapter coap_inst;
            CoAPAdapter::CoAPMessage coapmessage;
            auto st = coap_inst.parseRequest(std::string(buf.begin(), buf.end()), coapmessage);

            ///check if this is an ACK or not?
            if(coap_inst.getRequestType(static_cast<std::uint32_t>(coapmessage.coapheader.type)) == "Acknowledgement") {
                std::cout << basename(__FILE__) << ":" << __LINE__ << " This is an ACK" << std::endl;
            } else {
                ///Build the Response for a given Request
            }
            return(ret);
        }
    }
    
    return(-1);
}

std::int32_t App::tx(std::string& in) {
    struct sockaddr_in peerAddr;
    struct addrinfo *result;

    auto s = getaddrinfo(get_peerHost().data(), std::to_string(get_peerPort()).c_str(), nullptr, &result);
    if (!s) {
        peerAddr = *((struct sockaddr_in*)(result->ai_addr));
        freeaddrinfo(result);
    }

    socklen_t len = sizeof(peerAddr);
    size_t ret = sendto(serverFd, (const void *)in.data(), (size_t)in.length(), 0, (struct sockaddr *)&peerAddr, len);
    if(len < 0) {
        std::cout << basename(__FILE__) << ":" << __LINE__ << " Error: sendto peer failed" << std::endl;
        return(-1);
    }

    return(ret);
}

std::int32_t App::init(const Scheme_t& scheme) {
    epollFd = ::epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event evt;
    if(serverFd > 0) {
        evt.data.u64 = (serverFd << 32) | static_cast<std::uint32_t>(scheme);
        evt.events = EPOLLHUP | EPOLLIN;
        
        ::epoll_ctl(epollFd, EPOLL_CTL_ADD, serverFd, &evt);
        evts.push_back(evt);
    }
    return(0);
}

std::int32_t App::handle_io_coaps(const std::int32_t& fd) {
    auto ret = get_adapter().rx(fd);
    auto rsp = get_adapter().get_coapAdapter().getResponse();
    if(rsp.length()) {
        get_adapter().tx(rsp);
    }
}

std::int32_t App::handle_io_coap(const std::int32_t& fd) {
    rx(fd);
}

std::int32_t App::start(bool isCoAPs) {

    std::cout << basename(__FILE__) << ":" << __LINE__ << " evts.size: " << evts.size() << std::endl;
    std::vector<struct epoll_event> events(evts.size());

    if(isCoAPs) {
        get_adapter().connect();
    }

    for(;;) {

        if(!evts.size()) {
        }

        auto eventCount = ::epoll_wait(epollFd, events.data(), evts.size(), 9000);

        if(!eventCount) {
            /// Timeout of 9000ms happens.
           
        } else if(eventCount > 0) {
            ///event is received.
            events.resize(eventCount);
            for(auto it = events.begin(); it != events.end(); ++it) {

                struct epoll_event ent = *it;
                auto handle = ((ent.data.u64 >> 32) & 0xFFFFFFFF);
                auto scheme = ent.data.u64 & 0xFFFFFFFF;
                
                if(ent.events & EPOLLHUP) {
                    std::cout << "fn:" << __PRETTY_FUNCTION__ << " line:" << __LINE__ <<" ent.events: EPOLLHUP" << std::endl;
                }

                if(ent.events & EPOLLIN) {
                    std::cout << basename(__FILE__) << ":" << __LINE__ << " EPOLLIN on Fd: " << handle << std::endl;
                    if(scheme == CoAPs) {
                        handle_io_coaps(handle);
                    } else {
                        handle_io_coap(handle);
                    }
                }

                if(ent.events & EPOLLERR) {
                    ///Error on socket.
                    std::cout << "ent.events: EPOLLERR" << std::endl;
                }

                if(ent.events & EPOLLET) {
                    std::cout << "ent.events: EPOLLET" << std::endl;
                }
            }
        }
    }
}

std::int32_t App::stop() {
    std::cout << basename(__FILE__) << ":" << __LINE__ << " Not implemented yet" << std::endl;
    return(0);
}


#endif /*__app_cpp__*/