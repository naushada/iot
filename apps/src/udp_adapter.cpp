#ifndef __udp_adapter_cpp__
#define __udp_adapter_cpp__

#include "udp_adapter.hpp"
#include "app.hpp"

void UDPAdapter::hex_dump(const std::string& in) {
    std::stringstream ss;

    for(const auto& ent: in)
    {
        ss << std::hex << ent << ' ';
    }

    ss << std::dec;
    std::string hexDump = ss.str();
    std::cout << hexDump << std::endl;
}

std::int32_t UDPAdapter::rx(std::int32_t fd, std::string& out, std::string& peerIP, std::uint16_t& peerPort) {
    std::vector<std::uint8_t> request(1400);
    std::int32_t len;
    struct sockaddr_in session;
    memset(&session, 0, sizeof(session));
    socklen_t slen = sizeof(session);

    len = ::recvfrom(fd, request.data(), request.size(), MSG_TRUNC, (struct sockaddr *)&session, &slen);

    if(len < 0) {
        perror("recvfrom");
        return (-1);
    } else {
        request.resize(len);
        
        out.assign(std::string(request.begin(), request.end()));
        peerPort = ::ntohs(session.sin_port);
        peerIP = ::inet_ntoa(session.sin_addr);
        std::cout << basename(__FILE__) << ":" << __LINE__ << " got len: " << std::to_string(len) << " bytes from peer: " 
                  <<  peerIP << " port: " << std::to_string(peerPort) << std::endl;
        return(0);
    }
    return(-1);  
}

std::int32_t UDPAdapter::tx(const std::string& in, const ServiceType_t& service, const std::string& toIP, const std::uint16_t& toPort) {
    struct sockaddr_in peerAddr;
    struct addrinfo *result;

    auto it = std::find_if(app().services().begin(), app().services().end(), [&](const auto& ent) -> bool {return(service == ent.fist);});

    if(it != app().services().end()) {
        auto& inst = *it;
        std::cout << basename(__FILE__) << ":" << __LINE__ << " peerHost: " << toIP << " peerPort: " << toPort 
                  << " scheme: " << inst.second->scheme() << "service: " <<  service <<  std::endl;

        for(const auto& ent: in) {
            std::printf("%x ", (unsigned char)ent);
        }
        std::printf("\n");

        auto s = getaddrinfo(toIP.data(), std::to_string(toPort).c_str(), nullptr, &result);
        if (!s) {
            peerAddr = *((struct sockaddr_in*)(result->ai_addr));
            freeaddrinfo(result);
        } else {
            std::cout << "fn:"<<__PRETTY_FUNCTION__ << ":" << __LINE__ << " Error Unable to get addrinfo for service:" << service << " error: " 
                      << std::strerror(errno) << std::endl;
            return(-1); 
        }

        socklen_t len = sizeof(peerAddr);
        std::int32_t ret = sendto(inst.second->handle(), (const void *)in.data(), (size_t)in.length(), 0, (struct sockaddr *)&peerAddr, len);
        if(ret < 0) {
            std::cout << basename(__FILE__) << ":" << __LINE__ << " Error: sendto peer failed for Fd:" << inst.second->handle() << " toIP:" << toIP
                      << " localIP:" << inst.second->host() << " selfPort:" << std::to_string(inst.second->port())
                      << " peerPort:" << std::to_string(toPort) << std::endl;
            return(-1);
        }
    }
    return(0);
}

std::int32_t UDPAdapter::init(const std::string& sHost, const std::uint16_t& sPort, const Scheme_t& sch, const Role_t& r) {

    std::int32_t fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(fd < 0) {
        std::cout << "fn:"<<__PRETTY_FUNCTION__ << ":" << __LINE__ << " Error socket creation failed error:"<< std::strerror(errno) << std::endl; 
        return(-1);
    }

    struct sockaddr_in selfAddr;
    struct addrinfo *result;
    auto s = getaddrinfo(sHost.data(), std::to_string(sPort).c_str(), nullptr, &result);
    if (!s) {
        selfAddr = *((struct sockaddr_in*)(result->ai_addr));
        freeaddrinfo(result);
    } else {
        std::cout << "fn:"<<__PRETTY_FUNCTION__ << ":" << __LINE__ << " Error getaddrinfo failed error:"<< std::strerror(errno) << std::endl; 
        return(-1);
    }

    socklen_t len = sizeof(selfAddr);
    auto status = ::bind(fd, (struct sockaddr *)&selfAddr, len);
    if(status < 0) {
        std::cout << "fn:"<<__PRETTY_FUNCTION__ << ":" << __LINE__ << " bind failed error:"<< std::strerror(errno) << std::endl;
        return(-1);
    }
    
    host(sHost);
    port(sPort);
    handle(fd);
    scheme(sch);
    role(r);
    
    return(0);
}


std::int32_t UDPAdapter::start(Role_t role, Scheme_t scheme) {
    return(0);
}

std::int32_t UDPAdapter::stop() {
    std::cout << basename(__FILE__) << ":" << __LINE__ << " Not implemented yet" << std::endl;
    return(0);
}
































#endif /*__udp_adapter_cpp__*/