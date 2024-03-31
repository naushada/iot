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

#include "udp_adapter.hpp"

class App {

    public:
        enum class LwM2MBootstrapState: std::uint8_t {
            IDLE_ST,
            SECURITY_OBJECT_INSTANCE0_ST,
            SECURITY_OBJECT_INSTANCE1_ST,
            SERVER_OBJECT_INSTANCE0_ST,
            SERVER_OBJECT_INSTANCE1_ST,
            DONE_ST
        };

        enum class LwM2MDeviceManagementState: std::uint8_t {
            IDLE_ST,
            REGISTRATION_ST,
            REGISTRATION_UPDATE_ST,
            DONE_ST
        };

        App(std::string& host, std::uint16_t& port, UDPAdapter::Scheme_t& scheme, UDPAdapter::ServiceType_t& service) : 
            m_bsState(LwM2MBootstrapState::IDLE_ST), 
            m_dmState(LwM2MDeviceManagementState::IDLE_ST), 
            m_udpAdapter(std::make_shared<UDPAdapter>(host, port, scheme, service)) {
        }

        ~App() {
        }

        
        std::int32_t start(UDPAdapter::Role_t role, UDPAdapter::Scheme_t scheme);
        std::int32_t stop();

        std::shared_ptr<UDPAdapter>& udpAdapter() {
            return(m_udpAdapter);
        }
        

    private:
        LwM2MBootstrapState m_bsState;
        LwM2MDeviceManagementState m_dmState;
        std::shared_ptr<UDPAdapter> m_udpAdapter;
};

#endif /*__app_hpp__*/