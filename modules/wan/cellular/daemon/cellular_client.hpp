#ifndef __cellular_client_hpp__
#define __cellular_client_hpp__

#include <memory>
#include <string>

#include <ace/Event_Handler.h>

#include "data_store/client.hpp"
#include "cell_state.hpp"
#include "nmea_parser.hpp"
#include "at_parser.hpp"
#include "serial_channel.hpp"

/**
 * @file cellular_client.hpp
 * @brief The cellular-client daemon: reactor-driven modem + GNSS producer.
 *
 * Connects to ds, opens the AT (and optional GNSS) ttys as reactor-registered
 * SerialChannels, and on a periodic ACE timer polls the modem (+CSQ/+COPS?/
 * +CEREG?/+CGPADDR=1/+QCCID), routes the responses through the pure parsers
 * into a CellularState, and publishes cell.* / gps.* to the data-store. All I/O
 * is ACE; no raw POSIX. The connection lifecycle token (cell.state) is derived
 * from registration + assigned-IP each tick.
 */

namespace cellular {

class CellularClient : public ACE_Event_Handler {
    public:
        struct Config {
            std::string  ds_sock;                       ///< "" → ds default
            std::string  modem_tty = "/dev/ttyUSB2";
            std::string  gps_tty;                       ///< "" → no NMEA GNSS
            std::string  apn;
            unsigned     interval_sec = 30;
            bool         gps_enable = true;
        };

        explicit CellularClient(Config cfg) : m_cfg(std::move(cfg)) {}
        ~CellularClient() override = default;

        /// Connect ds, read config overrides, open channels, run the reactor.
        /// Returns a process exit code.
        int run();

        /// Poll timer: query the modem + publish the current state.
        int handle_timeout(const ACE_Time_Value&, const void*) override;

    private:
        void load_config_from_ds();
        void on_at_line(const std::string& line);
        void on_nmea_line(const std::string& line);
        void poll_modem();
        void publish();

        Config                          m_cfg;
        data_store::Client              m_ds;
        CellularState                   m_state;
        GpsFix                          m_gps;
        std::unique_ptr<SerialChannel>  m_at;
        std::unique_ptr<SerialChannel>  m_gnss;
        bool                            m_apn_sent = false;
        Reg                             m_lastReg = Reg::Unknown;
        bool                            m_haveIp = false;
        bool                            m_gps_via_at = false;  ///< GNSS over AT poll (no NMEA tty)
        Vendor                          m_vendor = Vendor::Generic;
        bool                            m_gps_started = false; ///< GNSS engine kicked
        unsigned                        m_poll_count = 0;      ///< for periodic GNSS restart
};

} // namespace cellular

#endif /*__cellular_client_hpp__*/
