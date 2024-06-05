#ifndef __app_cpp__
#define __app_cpp__

#include "app.hpp"

std::int32_t App::start(const bool& isInterruptted, const std::uint32_t& toInMs) {

    std::vector<struct epoll_event> events(m_evts.size());
    std::cout << basename(__FILE__) << ":" << __LINE__ << " events.size: " << events.size() << std::endl;

    for(auto& service: services()) {
        if((UDPAdapter::ServiceType_t::LwM2MClient == service.first) && (UDPAdapter::Scheme_t::CoAPs == service.second->scheme())) {
            service.second->dtlsAdapter()->connect(service.second->rHost(), service.second->rPort());
            break;
        }
    }

    for(; !isInterruptted ;) {

        ///@brief fallback to original size of vector
        events.resize(m_evts.size());

        auto eventCount = ::epoll_wait(m_epollFd, events.data(), m_evts.size(), toInMs);

        if(!eventCount) {
            /// Timeout of toInMs happens.
           
        } else if(eventCount > 0) {
            ///event is received.
            events.resize(eventCount);

            for(auto& event: events) {

                struct epoll_event ent = event;
                std::int32_t handle = (((ent.data.u64 & 0xFFFFFFFF00000000U) >> 32) & 0xFFFFFFFFU);
                UDPAdapter::ServiceType_t service = static_cast<UDPAdapter::ServiceType_t>(((ent.data.u64 & 0xFFFF0000U) >> 16) & 0xFFFFU);
                UDPAdapter::Scheme_t scheme = static_cast<UDPAdapter::Scheme_t>((ent.data.u64 & 0x0000FF00 >> 8) & 0xFFU);
                /// @brief last Octet is for Timer Service.

                if(ent.events & EPOLLHUP) {
                    /// @brief Peer has closed the connection
                    std::cout << "fn:" << __PRETTY_FUNCTION__ << " line:" << __LINE__ <<" ent.events: EPOLLHUP" << std::endl;
                }

                if(ent.events & EPOLLIN) {
                    /// @brief Data received from Peer
                    std::cout << basename(__FILE__) << ":" << __LINE__ << " EPOLLIN on Fd: " << handle << std::endl;
                    handle_io(handle, service, scheme);
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
    return(0);
}

std::int32_t App::init(const std::string& bsFile) {
    std::ifstream ifs(bsFile);
    std::stringstream ss("");

    if(ifs.is_open()) {
        ss << ifs.rdbuf();
        ifs.close();

        json bs = json::parse(ss.str());
        for(const auto& ent: bs) {
            for(const auto& [key, value]: ent.items()) {
                if(!key.compare("serial_no") && value.is_string()) {
                    std::shared_ptr<App::Device> dev;
                    device(value.get<std::string>(), dev);
                }
            }
        }
        return(0);
    }
    return(-1);

}

std::int32_t App::add_rx_handler(const UDPAdapter::ServiceType_t& service, const UDPAdapter::Scheme_t& scheme, std::shared_ptr<UDPAdapter> ent) {

    struct epoll_event evt;
    auto it = std::find_if(services().begin(), services().end(), [&](const auto& ent) -> bool {return(service == ent.first);});

    if(it != services().end()) {
        /// @brief release the ownership
        std::cout << basename(__FILE__) << ":" << __LINE__ << " Error: entry is already present for a given service:"<< service << std::endl;
        it->second.reset();
    }

    evt.data.u64 = (((static_cast<std::uint64_t>(ent->handle() & 0xFFFFFFFFU)) << 32) | 
                      static_cast<std::uint32_t>(((service & 0xFFFFU) << 16) | ((scheme & 0xFFU) << 8) | 0xFF));

    evt.events = EPOLLHUP | EPOLLIN;
    std::int32_t channel = ent->handle();
    ::epoll_ctl(m_epollFd, EPOLL_CTL_ADD, channel, &evt);
    m_evts.push_back(evt);

    if(!(services().insert(std::pair<UDPAdapter::ServiceType_t, std::shared_ptr<UDPAdapter>>(service, ent)).second)) {
        std::cout << basename(__FILE__) << ":" << __LINE__ << " Error: This shouldn't have happened" << std::endl;
    }
    return(0);
}

std::int32_t App::handle_io(const std::int32_t& fd, const UDPAdapter::ServiceType_t&  service, const UDPAdapter::Scheme_t& scheme) {
    std::cout << basename(__FILE__) << ":" << __LINE__ << " handle:" << std::to_string(fd) << " service:" << std::to_string(service) << " scheme:" 
              << std::to_string(scheme) << std::endl;

    switch (scheme) {
        case UDPAdapter::Scheme_t::CoAP:
        {
            handle_io_coap(fd, service);
        }
        break;
        case UDPAdapter::Scheme_t::CoAPs:
        {
            handle_io_coaps(fd, service);
        }
        break;
        default:
            std::cout << "fn:" << __PRETTY_FUNCTION__ << ":" << __LINE__ << " Error unknown scheme:" << std::to_string(scheme) << std::endl;
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
                bool isAmIClient = (ctx->service() == UDPAdapter::ServiceType_t::DeviceMgmtClient)? true: false;
                ctx->coapAdapter()->processRequest(isAmIClient, in, responses);
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

std::int32_t App::handle_io_coaps(const std::int32_t& fd, const UDPAdapter::ServiceType_t& service) {
    auto it = std::find_if(services().begin(), services().end(), [&](const auto& ent) -> bool {return(service == ent.first);});

    if(it != services().end()) {
        std::string IP;
        std::uint16_t port;
        auto& inst = it->second;
        std::string rawData;
        //auto ret = inst->dtlsAdapter()->rx(fd, IP, port);
        auto ret = inst->rx(fd, rawData, IP, port);
        if(ret) {
            /// @brief Error in recvfrom
            return(-1);
        }

        std::string deciphered;
        deciphered = inst->dtlsAdapter()->decipher(rawData, IP, port);

        if(deciphered.length()) {
            /// @brief rawData is deciphered successfully.
        }

        return(0);
    }
    return(-1);
}

std::int32_t App::handle_io_coap(const std::int32_t& fd, const UDPAdapter::ServiceType_t& service) {
    //std::cout << basename(__FILE__) << ":" << __LINE__ << " received packet handle_io_coap" << std::endl;
    auto it = std::find_if(services().begin(), services().end(), [&](auto& ent) -> bool {return(service == ent.second->service());});

    if(it != services().end()) {
        std::cout << basename(__FILE__) << ":" << __LINE__ << " received packet handle_io_coap service: " << service << std::endl;
        auto& ctx = it->second;
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
            bool isAmIClient = false;
            ret = ctx.second->coapAdapter()->processRequest(isAmIClient, ss.str(), responses);
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

#endif /*__app_cpp__*/