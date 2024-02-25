#ifndef __app_cpp__
#define __app_cpp__

#include "app.hpp"

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

std::int32_t App::init() {
    epollFd = ::epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event evt;
    if(serverFd > 0) {
        evt.data.fd = serverFd;
        evt.events = EPOLLHUP | EPOLLIN;
        
        ::epoll_ctl(epollFd, EPOLL_CTL_ADD, serverFd, &evt);
        evts.push_back(evt);
    }
    return(0);
}

std::int32_t App::handle_io(std::int32_t fd) {
    auto ret = get_adapter().rx(fd);
    auto rsp = get_adapter().get_coapAdapter().getResponse();
    if(rsp.length()) {
        get_adapter().tx(rsp);
    }
}

std::int32_t App::start() {

    std::cout << basename(__FILE__) << ":" << __LINE__ << " evts.size: " << evts.size() << std::endl;
    std::vector<struct epoll_event> events(evts.size());
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
                auto handle = ent.data.fd;
                
                if(ent.events & EPOLLHUP) {
                    std::cout << "fn:" << __PRETTY_FUNCTION__ << " line:" << __LINE__ <<" ent.events: EPOLLHUP" << std::endl;
                }
                if(ent.events & EPOLLIN) {
                    std::cout << basename(__FILE__) << ":" << __LINE__ << " EPOLLIN on Fd: " << handle << std::endl;
                    handle_io(handle);
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