#ifndef __vehicle_client_hpp__
#define __vehicle_client_hpp__

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <ace/Event_Handler.h>

#include "data_store/client.hpp"

#include "can_socket.hpp"

/// iot-vehicled — reactor-driven OBD-II (ISO 15765-4) telemetry producer.
///
/// Opens the SocketCAN interface as a reactor-registered CanSocket, and on a
/// periodic ACE timer sends the next Mode 01 PID request (round-robin over the
/// supported set) on the functional id 0x7DF. ECU responses arrive on the
/// reactor thread, are decoded by the pure vehicle::obd core, and published to
/// the data-store as VOLATILE `vehicle.*` keys (transient per-tick readings —
/// no SD-card fsync). `vehicle.link` reflects bus health. All I/O is ACE; the
/// only raw-POSIX is opening the CAN fd, which is then driven by the reactor.

namespace vehicle {

class VehicleClient : public ACE_Event_Handler {
    public:
        struct Config {
            std::string  ds_sock;              ///< "" → ds default socket
            std::string  iface       = "can0";
            unsigned     interval_ms = 1000;
        };

        explicit VehicleClient(Config cfg) : m_cfg(std::move(cfg)) {}
        ~VehicleClient() override = default;

        /// Connect ds, read config overrides, open the CAN socket, run the
        /// reactor. Returns a process exit code.
        int run();

        /// Poll timer: send the next PID request + refresh vehicle.link.
        int handle_timeout(const ACE_Time_Value&, const void*) override;

    private:
        void load_config_from_ds();
        void on_frame(std::uint32_t id, const std::uint8_t* data, std::uint8_t dlc);
        void publish_link(const char* state);

        Config                       m_cfg;
        data_store::Client           m_ds;
        std::unique_ptr<CanSocket>   m_can;
        std::vector<std::uint8_t>    m_pids;        ///< round-robin poll list
        std::size_t                  m_next = 0;    ///< index into m_pids
        bool                         m_any_reply = false;
        std::string                  m_link;        ///< last published link state
};

} // namespace vehicle

#endif /*__vehicle_client_hpp__*/
