#ifndef __udp_adapter_cpp__
#define __udp_adapter_cpp__

#include "udp_adapter.hpp"

void UDPAdapter::hex_dump(const std::string& in) {
    std::stringstream oss;
    for(const auto& ent: in)
    {
        oss << ent << ' ';
    }

    std::string hexDump = oss.str();
    std::cout << hexDump << std::endl;
}

std::int32_t UDPAdapter::rx(std::int32_t fd, std::string& out, std::uint32_t& peerIP, std::uint16_t& peerPort) {
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
        peerIP = session.sin_addr.s_addr;
        return(0);
    }
    return(-1);  
}

std::int32_t UDPAdapter::tx(std::string& in, ServiceType_t& service) {
    struct sockaddr_in peerAddr;
    struct addrinfo *result;

    auto it = std::find_if(services().begin(), services().end(), [&](auto& ent) -> bool {
        return(service == ent.second->service());
    });

    if(it != services().end()) {
        auto& ctx = *it;
        std::cout << basename(__FILE__) << ":" << __LINE__ << " peerHost: " << ctx.second->peerHost() << " peerPort: " << ctx.second->peerPort() << " service: " << service << std::endl;

        for(const auto& ent: in) {
            printf("%x ", (unsigned char)ent);
        }
        printf("\n");

        auto s = getaddrinfo(ctx.second->peerHost().data(), std::to_string(ctx.second->peerPort()).c_str(), nullptr, &result);
        if (!s) {
            peerAddr = *((struct sockaddr_in*)(result->ai_addr));
            freeaddrinfo(result);
        } else {
            std::cout << "fn:"<<__PRETTY_FUNCTION__ << ":" << __LINE__ << " Error Unable to get addrinfo for bs:"<< std::strerror(errno) << std::endl;
            return(-1); 
        }

        socklen_t len = sizeof(peerAddr);
        std::int32_t ret = sendto(ctx.second->fd(), (const void *)in.data(), (size_t)in.length(), 0, (struct sockaddr *)&peerAddr, len);
        if(ret < 0) {
            std::cout << basename(__FILE__) << ":" << __LINE__ << " Error: sendto peer failed for Fd:" << ctx.second->fd() << " bs:" << ctx.second->peerHost()
                      << " localIP:" << ctx.second->selfHost() << " selfPort:" << std::to_string(ctx.second->selfPort())
                      << " peerPort:" << std::to_string(ctx.second->peerPort()) << std::endl;
            return(-1);
        }
    }
    return(0);
}

std::int32_t UDPAdapter::init(const std::string& host, const std::uint16_t& port, const Scheme_t& scheme, const ServiceType_t& service) {
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
    ctx->selfHost(host);
    ctx->selfPort(port);
    ctx->service(service);

    if(!services().insert(std::pair(service, std::move(ctx))).second) {
        ///Insertion failed.
        std::cout << "fn:" << __PRETTY_FUNCTION__ << ":" << __LINE__ << " Error Failed to add into services map" << std::endl;
        return(-1);
    }
    return(0);
}

std::int32_t UDPAdapter::init(const std::string& host, const std::uint16_t& port, const Scheme_t& scheme) {
    auto ret = init(host, port, scheme, ServiceType_t::LwM2MClient);
    if(ret) {
        std::cout << "fn:" << __PRETTY_FUNCTION__ << ":" << __LINE__ << " init failed for scheme:" << scheme << std::endl;
        return(ret);
    }
    return(ret);
}

std::int32_t UDPAdapter::add_event_handle(const Scheme_t& scheme, const ServiceType_t& svc) {

    struct epoll_event evt;
    auto it = std::find_if(services().begin(), services().end(), [&](auto& ent) -> bool {
        return(svc == ent.second->service());
    });

    if(it != services().end()) {
        auto& ent = *it;
        evt.data.u64 = (((static_cast<std::uint64_t>(ent.second->fd())) << 32) | 
                         static_cast<std::uint32_t>(((svc & 0xFFFF) << 16) | (scheme & 0xFFFF)));
        evt.events = EPOLLHUP | EPOLLIN;
        ::epoll_ctl(m_epollFd, EPOLL_CTL_ADD, ent.second->fd(), &evt);
        m_evts.push_back(evt);
        return(0);
    }
    return(-1);
}

std::int32_t UDPAdapter::handle_io(const std::int32_t& fd, const Scheme_t& scheme, const ServiceType_t&  service) {
    switch (scheme) {
        case UDPAdapter::Scheme_t::CoAP:
        {
            //std::cout << basename(__FILE__) << ":" << __LINE__ << " scheme: "<< scheme << " service: " << service << std::endl;
            handle_io_coap(fd, service);
        }
        break;
        case UDPAdapter::Scheme_t::CoAPs:
        {
            handle_io_coaps(fd, service);
        }
        break;
        default:
            std::cout << "fn:" << __PRETTY_FUNCTION__ << ":" << __LINE__ << " Error unknown scheme:" << scheme << std::endl;
        break;
    }
}

std::int32_t UDPAdapter::process_request(const std::string& in, const std::unique_ptr<UDPAdapter::ServiceContext_t>& ctx, CoAPAdapter::CoAPMessage& message) {
    if(UDPAdapter::Scheme_t::CoAP == ctx->scheme()) {
        //ctx->coapAdapter()->parseRequest(in, message);
        std::string uris;
        std::uint32_t oid, oiid, rid, riid;
        std::vector<std::string> responses;

        if(!ctx->coapAdapter()->getRequestType(message.coapheader.type).compare("Acknowledgement")) {
            ctx->coapAdapter()->dumpCoAPMessage(message);
        } else {
            if(ctx->coapAdapter()->isCoAPUri(message, uris)) {

                ctx->coapAdapter()->processRequest(in, responses);
                for(auto& response: responses) {
                    tx(response, ctx->service());
                }

            } else if(ctx->coapAdapter()->isLwm2mUri(message, uris, oid, oiid, rid, riid)) {

                LwM2MObject object;
                LwM2MObjectData data;
                if(uris.empty()) {
                    object.m_oid = oid;
                    data.m_oiid = oiid;
                    data.m_rid = rid;
                    data.m_riid = riid;
                    ctx->coapAdapter()->lwm2mAdapter()->parseLwM2MObjects(message.payload, data, object);
                }
            }
        }
    } else {
        //ctx->dtlsAdapter()->rx(ctx->fd());
    }
    return(0);
}

std::int32_t UDPAdapter::handle_io_coaps(const std::int32_t& fd, const ServiceType_t& service) {
    auto it = std::find_if(services().begin(), services().end(), [&](auto& ent) -> bool {
        return(service == ent.second->service());
    });

    if(it != services().end()) {

        auto& ctx = *it;
        ctx.second->dtlsAdapter()->rx(fd);
        auto rsp = ctx.second->dtlsAdapter()->responses();

        for(auto& response: ctx.second->dtlsAdapter()->responses()) {
            ctx.second->dtlsAdapter()->tx(response);
        }
        return(0);
    }
    return(-1);
}

std::int32_t UDPAdapter::handle_io_coap(const std::int32_t& fd, const ServiceType_t& service) {
    //std::cout << basename(__FILE__) << ":" << __LINE__ << " received packet handle_io_coap" << std::endl;
    auto it = std::find_if(services().begin(), services().end(), [&](auto& ent) -> bool {
        return(service == ent.second->service());
    });

    if(it != services().end()) {
        std::cout << basename(__FILE__) << ":" << __LINE__ << " received packet handle_io_coap service: " << service << std::endl;
        auto& ctx = *it;
        std::uint32_t peerIP;
        std::uint16_t peerPort;
        std::string out;
        std::stringstream ss;
        std::int32_t ret;
        std::vector<std::string> responses;
        CoAPAdapter::CoAPMessage message;
        ss.str("");

        do {

            ret = rx(fd, out, peerIP, peerPort);
            if(!ret) {
                ctx.second->peerPort(peerPort);
                struct in_addr pp;
                pp.s_addr = peerIP;
                ctx.second->peerHost(inet_ntoa(pp));
                ctx.second->coapAdapter()->parseRequest(out, message);
                ss << out;
                //process_request(out, ctx.second, message);
            }

        } while(message.ismorebitset);

        if(UDPAdapter::ServiceType_t::LwM2MClient == service) {
            //CoAPAdapter::CoAPMessage coapmessage;
            //auto ret = ctx.second->coapAdapter()->parseRequest(out, coapmessage);

            for(const auto& ent: out) {
                printf("%x ", (unsigned char)ent);
            }
            printf("\n");
            auto response = ctx.second->coapAdapter()->buildResponse(message);
            if(response.length()) {
                tx(response, ctx.second->service());
            }

        } else {

            ret = ctx.second->coapAdapter()->processRequest(ss.str(), responses);
            if(responses.size()) {
                for(auto& response: responses) {
                    std::cout << basename(__FILE__) << ":" << __LINE__ << " servie: " << ctx.second->service() << std::endl;
                    tx(response, ctx.second->service());
                }
            }
        }
        return(0);
    }
    //rx(fd);
    return(0);
}

std::int32_t UDPAdapter::start(Role_t role, Scheme_t scheme) {

    std::cout << basename(__FILE__) << ":" << __LINE__ << " evts.size: " << m_evts.size() << std::endl;
    std::vector<struct epoll_event> events(m_evts.size());

    if(UDPAdapter::Role_t::CLIENT == role &&  UDPAdapter::Scheme_t::CoAPs == scheme) {
        auto it = std::find_if(services().begin(), services().end(), [&](auto& ent) -> bool {
            return(UDPAdapter::ServiceType_t::LwM2MClient == ent.second->service());
        });

        if(it != services().end()) {
            auto& ent = *it;
            ent.second->dtlsAdapter()->connect(ent.second->peerHost(), ent.second->peerPort());
        }
    }

    for(;;) {

        if(!m_evts.size()) {
        }

        auto eventCount = ::epoll_wait(m_epollFd, events.data(), m_evts.size(), 9000);

        if(!eventCount) {
            /// Timeout of 9000ms happens.
           
        } else if(eventCount > 0) {
            ///event is received.
            events.resize(eventCount);
            //for(auto it = events.begin(); it != events.end(); ++it) {
            for(auto& event: events) {
                struct epoll_event ent = event;
                std::int32_t handle = ((ent.data.u64 >> 32) & 0xFFFFFFFF);
                ServiceType_t service = static_cast<ServiceType_t>((ent.data.u64 >> 16) & 0xFFFF);
                Scheme_t scheme = static_cast<Scheme_t>(ent.data.u64 & 0xFFFF);

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

std::int32_t UDPAdapter::stop() {
    std::cout << basename(__FILE__) << ":" << __LINE__ << " Not implemented yet" << std::endl;
    return(0);
}
































#endif /*__udp_adapter_cpp__*/