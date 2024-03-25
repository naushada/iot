#ifndef __app_cpp__
#define __app_cpp__

#include "app.hpp"
#include "coap_adapter.hpp"
#include "lwm2m_adapter.hpp"

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

/*
CoAPAdapter& App::get_coapAdapter() {
    return(*coapAdapter.get());
}

LwM2MAdapter& App::get_lwm2mAdapter() {
    return(*lwm2mAdapter.get());
}

*/

std::int32_t App::rx(std::int32_t fd, std::string& out, std::uint32_t& peerIP, std::uint16_t& peerPort) {
    std::vector<std::uint8_t> buf(1400);
    std::int32_t len;
    struct sockaddr_in session;
    memset(&session, 0, sizeof(session));
    socklen_t slen = sizeof(session);

    len = recvfrom(fd, buf.data(), buf.size(), MSG_TRUNC, (struct sockaddr *)&session, &slen);
    

    if(len < 0) {
        perror("recvfrom");
        return (-1);
    } else {
        buf.resize(len);
        std::cout << basename(__FILE__) << ":" << __LINE__ << " got len: " << len << " bytes from port: " << ntohs(session.sin_port) << std::endl;
        out.assign(std::string(buf.begin(), buf.end()));
        peerPort = ntohs(session.sin_port);
        peerIP = ntohl(session.sin_addr.s_addr);
        return(0);
    }
    return(-1);  
}

std::int32_t App::rx(std::int32_t fd) {
#if 0
    std::int32_t ret = -1;
    std::vector<std::uint8_t> buf(1400);
    std::int32_t len;
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
            //CoAPAdapter coap_inst;
            CoAPAdapter::CoAPMessage coapmessage;
            auto st = get_coapAdapter().parseRequest(std::string(buf.begin(), buf.end()), coapmessage);

            ///check if this is an ACK or not?
            if(!st && get_coapAdapter().getRequestType(static_cast<std::uint32_t>(coapmessage.coapheader.type)) == "Acknowledgement") {
                //std::cout << basename(__FILE__) << ":" << __LINE__ << " This is an ACK" << std::endl;
                get_coapAdapter().dumpCoAPMessage(coapmessage);
            } else {

                ///Build the Response for a given Request
                std::string uri;
                std::uint32_t oid, oiid, rid, riid;
                if(get_coapAdapter().isCoAPUri(coapmessage, uri)) {
                    ///This is a CoAP URI handle it.
                } else if(get_coapAdapter().isLwm2mUri(coapmessage, uri)) {
                    /// This is aLwM2M string URI rd or bs
                    std::cout << basename(__FILE__) << ":" << __LINE__ << " This is either bs  or rd" << std::endl;
                    auto rsp = get_coapAdapter().buildResponse(coapmessage);
                    auto ret = sendto(fd, (const void *)rsp.data(), rsp.length(), 0, (struct sockaddr *)&session, slen);
                    std::cout << basename(__FILE__) << ":" << __LINE__ << " bs or rd response is sent ret:" << ret << std::endl;

                } else if(get_coapAdapter().isLwm2mUriObject(coapmessage, oid, oiid, rid, riid)) {
                    /// This is LwM2M Object URI, Handle it.
                    //LwM2MAdapter lwm2mAdapter;
                    LwM2MObjectData data;
                    LwM2MObject object;
                    data.m_oiid = oiid;
                    data.m_rid = rid;
                    data.m_riid = riid;
                    object.m_oid = oid;

                    if(!get_lwm2mAdapter().parseLwM2MObjects(coapmessage.payload, data, object)) {

                        ///Objects are extracted successfully
                        for(const auto& ent: object.m_value) {
                            std::cout << basename(__FILE__) << ":" << __LINE__ <<  " object.m_oid:" << object.m_oid <<" ent.m_oiid:" << ent.m_oiid << " ent.m_riid:" << ent.m_riid
                                      << " ent.m_rid:" << get_lwm2mAdapter().resourceIDName(oid, ent.m_rid) << " ent.m_ridlength:" << ent.m_ridlength << " ent.m_ridvalue.size:" << ent.m_ridvalue.size()
                                      << " ent.m_ridvalue:";
        
                            for(const auto& elm: ent.m_ridvalue) {
                                printf("%0.2X ", (std::uint8_t)elm);
                            }
                            printf("\n");
                        }
                        
                        ///Build Response and send it.
                        
                    }
                } else {
                    ///Unknown URI ---- Error 
                }
            }

            return(ret);
        }
    }
#endif
    return(-1);
}

std::int32_t App::tx(std::string& in, ServiceType_t& service) {
    struct sockaddr_in peerAddr;
    struct addrinfo *result;
    
    auto it = std::find_if(services.begin(), services.end(), [&](auto& ent) -> bool {
        return(service == ent.second->get_service());
    });

    if(it != services.end()) {
        auto& ctx = *it;
    
        auto s = getaddrinfo(ctx.second->get_peerHost().data(), std::to_string(ctx.second->get_peerPort()).c_str(), nullptr, &result);
        if (!s) {
            peerAddr = *((struct sockaddr_in*)(result->ai_addr));
            freeaddrinfo(result);
        } else {
            std::cout << "fn:"<<__PRETTY_FUNCTION__ << ":" << __LINE__ << " Error Unable to get addrinfo for bs:"<< std::strerror(errno) << std::endl;
            return(-1); 
        }

        socklen_t len = sizeof(peerAddr);
        std::int32_t ret = sendto(ctx.second->get_fd(), (const void *)in.data(), (size_t)in.length(), 0, (struct sockaddr *)&peerAddr, len);
        if(ret < 0) {
            std::cout << basename(__FILE__) << ":" << __LINE__ << " Error: sendto peer failed for Fd:" << ctx.second->get_fd() << " bs:" << ctx.second->get_peerHost()
                    << " localIP:" << ctx.second->get_selfHost() << " selfPort:" << std::to_string(ctx.second->get_selfPort())
                    << " peerPort:" << std::to_string(ctx.second->get_peerPort()) << std::endl;
            return(-1);
        }
        return(ret);
    }

    return(0);
}

std::int32_t App::init(const std::string& host, const std::uint16_t& port, const Scheme_t& scheme, const ServiceType_t& service) {
    std::int32_t fd = ::socket(AF_INET, SOCK_DGRAM, 0);
            
    if(fd < 0) {
        std::cout << "fn:"<<__PRETTY_FUNCTION__ << ":" << __LINE__ << " Error socket creation failed error:"<< std::strerror(errno) << std::endl; 
        return(-1);
    }
            
    struct sockaddr_in selfAddr;
    struct addrinfo *result;

    auto s = getaddrinfo(host.data(), std::to_string(port).c_str(), nullptr, &result);
    if (!s) {
        selfAddr = *((struct sockaddr_in*)(result->ai_addr));
        freeaddrinfo(result);
    } else {
        ::close(fd);
        std::cout << "fn:"<<__PRETTY_FUNCTION__ << ":" << __LINE__ << " Error getaddrinfo failed error:"<< std::strerror(errno) << std::endl; 
        return(-1);
    }

    socklen_t len = sizeof(selfAddr);
    auto status = ::bind(fd, (struct sockaddr *)&selfAddr, len);
    if(status < 0) {
        std::cout << "fn:"<<__PRETTY_FUNCTION__ << ":" << __LINE__ << " bind failed error:"<< std::strerror(errno) << std::endl;
        return(-1);
    }
                        
    std::cout << "fn:" << __PRETTY_FUNCTION__ << ":" << __LINE__ << " created handle:" << fd  << " for service:" << service << std::endl;

    std::unique_ptr<ServiceContext_t> ctx = std::make_unique<ServiceContext_t>(fd, scheme);
    ctx->set_selfHost(host);
    ctx->set_selfPort(port);
    ctx->set_service(service);

    if(!services.insert(std::pair(service, std::move(ctx))).second) {
        ///Insertion failed.
        std::cout << "fn:" << __PRETTY_FUNCTION__ << ":" << __LINE__ << " Error Failed to add into services map" << std::endl;
        return(-1);
    }
#if 0
    auto it = std::find_if(services.begin(), services.end(), [&](auto& ent) -> bool {
        return(fd == ent.second.get_fd());
    });
    if(it != services.end()) {
        auto& elm = *it;
        elm.second.set_selfHost(host);
        elm.second.set_selfPort(port);
        elm.second.set_service(service);
    }
#endif
    return(0);
}

std::int32_t App::init(const std::string& host, const std::uint16_t& port, const Scheme_t& scheme) {
    auto ret = init(host, port, scheme, ServiceType_t::LwM2MClient);
    if(ret) {
        std::cout << "fn:" << __PRETTY_FUNCTION__ << ":" << __LINE__ << " init failed for scheme:" << scheme << std::endl;
        return(ret);
    }

    return(ret);
}

std::int32_t App::add_event_handle(const Scheme_t& scheme, const ServiceType_t& svc) {
    
    struct epoll_event evt;
    auto it = std::find_if(services.begin(), services.end(), [&](auto& ent) -> bool {
        return(svc == ent.second->get_service());
    });

    if(it != services.end()) {
        auto& ent = *it;
        evt.data.u64 = (((static_cast<std::uint64_t>(ent.second->get_fd())) << 32) | static_cast<std::uint32_t>(((svc << 16) & 0xFFFF) | (scheme & 0xFFFF)));
        evt.events = EPOLLHUP | EPOLLIN;
        
        ::epoll_ctl(epollFd, EPOLL_CTL_ADD, ent.second->get_fd(), &evt);
        evts.push_back(evt);
        return(0);
    }

    return(-1);
}

std::int32_t App::handle_io(const std::int32_t& fd, const Scheme_t& scheme, const ServiceType_t&  service) {
    switch (scheme) {
        case App::CoAP:
        {
            handle_io_coap(fd, service);
        }
        break;
        case App::CoAPs:
        {
            handle_io_coaps(fd, service);
        }
        break;
    
        default:
            std::cout << "fn:" << __PRETTY_FUNCTION__ << ":" << __LINE__ << " Error unknown scheme:" << scheme << std::endl;
        break;
    }
}

std::int32_t App::handle_io_coaps(const std::int32_t& fd, const ServiceType_t& service) {
    auto it = std::find_if(services.begin(), services.end(), [&](auto& ent) -> bool {
        return(service == ent.second->get_service());
    });

    if(it != services.end()) {
        auto& ctx = *it;
        auto ret = ctx.second->get_dtls_adapter().rx(fd);
        auto rsp = ctx.second->get_dtls_adapter().get_coapAdapter().getResponse();
        if(rsp.length()) {
            ctx.second->get_dtls_adapter().tx(rsp);
        }
        return(0);
    }
    return(-1);
}

std::int32_t App::handle_io_coap(const std::int32_t& fd, const ServiceType_t& service) {
    std::cout << basename(__FILE__) << ":" << __LINE__ << " received packet handle_io_coap" << std::endl;
    auto it = std::find_if(services.begin(), services.end(), [&](auto& ent) -> bool {
        return(service == ent.second->get_service());
    });

    if(it != services.end()) {
        auto& ctx = *it;
        std::uint32_t peerIP;
        std::uint16_t peerPort;
        std::string out;
        std::vector<std::string> responses;
        auto ret = rx(fd, out, peerIP, peerPort);
        if(!ret) {
            ctx.second->set_peerPort(peerPort);
            struct in_addr pp;
            pp.s_addr = peerIP;
            ctx.second->set_peerHost(inet_ntoa(pp));
        }
        ret = ctx.second->get_coap_adapter().processRequest(out, responses);
        if(responses.size()) {
            //ctx.second->get_dtls_adapter().tx(rsp);
            for(auto& response: responses) {
                tx(response, ctx.second->get_service());
            }
        }
        return(0);
    }
    
    //rx(fd);
    return(0);
}

std::int32_t App::start(Role_t role, Scheme_t scheme) {

    std::cout << basename(__FILE__) << ":" << __LINE__ << " evts.size: " << evts.size() << std::endl;
    std::vector<struct epoll_event> events(evts.size());

    if(App::CLIENT == role &&  App::CoAPs == scheme) {
        auto it = std::find_if(get_services().begin(), get_services().end(), [&](auto& ent) -> bool {
            return(App::ServiceType_t::LwM2MClient == ent.second->get_service());
        });

        if(it != get_services().end()) {
            auto& ent = *it;
            ent.second->get_dtls_adapter().connect();
        }
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
                std::int32_t handle = ((ent.data.u64 >> 32) & 0xFFFFFFFF);
                Scheme_t scheme = static_cast<Scheme_t>(ent.data.u64 & 0xFFFF);
                ServiceType_t service = static_cast<ServiceType_t>((ent.data.u64 > 16) & 0xFFFF);
                
                if(ent.events & EPOLLHUP) {
                    std::cout << "fn:" << __PRETTY_FUNCTION__ << " line:" << __LINE__ <<" ent.events: EPOLLHUP" << std::endl;
                }

                if(ent.events & EPOLLIN) {
                    std::cout << basename(__FILE__) << ":" << __LINE__ << " EPOLLIN on Fd: " << handle << std::endl;
                    handle_io(handle, scheme, service);
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
    return(0);
}

std::int32_t App::stop() {
    std::cout << basename(__FILE__) << ":" << __LINE__ << " Not implemented yet" << std::endl;
    return(0);
}


#endif /*__app_cpp__*/