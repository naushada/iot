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
        using SerialNumber = std::string;
        using Identity = std::string;
        using Secret = std::string;

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
            CONNECTED_ST,
            CONNECTION_ERROR_ST,
            DONE_ST
        };

        struct Device {
            std::string m_serial;
            std::string m_ip;
            std::uint16_t m_port;
            std::uint32_t m_lt;
            std::string  m_lwm2m_version;
            std::string m_binding;
            LwM2MBootstrapState m_bsState;
            LwM2MDeviceManagementState m_dmState;

            Device() : m_serial(), m_ip(), m_port(), m_lt(1500), m_lwm2m_version("1.0"), m_binding("UQ"),
                        m_bsState(LwM2MBootstrapState::IDLE_ST), 
                        m_dmState(LwM2MDeviceManagementState::IDLE_ST) {}

            virtual ~Device() = default;
            
            void serial(std::string s) {
                m_serial = s;
            }
            const std::string& serial() const {
                return(m_serial);
            }

            void ip(auto ip) {
                m_ip = ip;
            }
            const auto& ip() const {
                return(m_ip);
            }

            void port(auto port) {
                m_port = port;
            }
            const auto& port() const {
                return(m_port);
            }

            void lt(auto lt) {
                m_lt = lt;
            }
            const auto& lt() const {
                return(m_lt);
            }

            void lwm2m_version(auto version) {
                m_lwm2m_version = version;
            }
            const auto& lwm2m_version() const {
                return(m_lwm2m_version);
            }

            void binding(auto b) {
                m_binding = b;
            }
            const auto& binding() const {
                return(m_binding);
            }

        };

        App(std::string& host, std::uint16_t& port, UDPAdapter::Scheme_t& scheme, UDPAdapter::ServiceType_t& service) : 
            m_udpAdapter(std::make_shared<UDPAdapter>(host, port, scheme, service, this)) {
        }
        
        App() = delete;
        ~App() {
        }

        
        std::int32_t start(UDPAdapter::Role_t role, UDPAdapter::Scheme_t scheme);
        std::int32_t stop();
        std::int32_t init(const std::string& bsConfig = "../config/bs/bs.json");

        auto& udpAdapter() {
            return(m_udpAdapter);
        }

        std::shared_ptr<Device> device(const SerialNumber& srNo) {
            auto it = std::find_if(m_devices.begin(),  m_devices.end(), [&](const auto& ent) -> bool {return(srNo == ent.first);});
            
            if(it != m_devices.end()) {
                return(it->second);
            }
            return(nullptr);
        }

        void device(SerialNumber srNo, std::shared_ptr<Device> ent) {
            if(!m_devices.insert(std::pair<SerialNumber, std::shared_ptr<Device>>(srNo, ent)).second) {
                /// @brief Insertion Failed
            }
        }

        Secret secret(const Identity& iden) {
            auto it = std::find_if(m_credentials.begin(), m_credentials.end(), [&](const auto& ent) -> bool { return(iden == ent.first);});
            if(it != m_credentials.end()) {
                return(it->second.first);
            }
            return(std::string());
        }

    private:
        std::shared_ptr<UDPAdapter> m_udpAdapter;
        std::unordered_map<SerialNumber, std::shared_ptr<Device>> m_devices;
        std::unordered_map<Identity, std::pair<Secret, SerialNumber>> m_credentials; 
};

#endif /*__app_hpp__*/