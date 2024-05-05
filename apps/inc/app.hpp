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

        struct Devices {
            std::string m_serial;
            std::vector<std::uint8_t> m_identity;
            std::vector<std::uint8_t> m_secret;
            std::string m_ip;
            std::uint16_t m_port;
            std::uint32_t m_lt;
            std::string  m_lwm2m_version;
            std::string m_binding;
            Devices() : m_serial(), m_identity(16), m_secret(16), m_ip(), m_port(), m_lt(1500), m_lwm2m_version("1.0"), m_binding("UQ") {}
            ~Devices() = default;
            
            void serial(std::string s) {
                m_serial = s;
            }
            const std::string& serial() const {
                return(m_serial);
            }

            void identity(auto iden) {
                m_identity = iden;
            }
            const auto& identity() const {
                return(m_identity);
            }

            void secret(auto iden) {
                m_secret = iden;
            }
            const auto& secret() const {
                return(m_secret);
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
            m_bsState(LwM2MBootstrapState::IDLE_ST), 
            m_dmState(LwM2MDeviceManagementState::IDLE_ST), 
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

        auto& devices() {
            return(m_devices);
        }
        

    private:
        LwM2MBootstrapState m_bsState;
        LwM2MDeviceManagementState m_dmState;
        std::shared_ptr<UDPAdapter> m_udpAdapter;
        std::unordered_map<std::string, std::unique_ptr<Devices>> m_devices;
};

#endif /*__app_hpp__*/