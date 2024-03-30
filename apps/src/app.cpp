#ifndef __app_cpp__
#define __app_cpp__

#include "app.hpp"
#include "udp_adapter.hpp"


std::int32_t App::start(UDPAdapter::Role_t role, UDPAdapter::Scheme_t scheme) {
    udpAdapter()->start(role, scheme);
    return(0);
}

std::int32_t App::stop() {
    return(0);
}

#endif /*__app_cpp__*/