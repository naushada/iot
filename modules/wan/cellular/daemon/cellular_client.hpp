#ifndef __cellular_client_hpp__
#define __cellular_client_hpp__

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <ace/Event_Handler.h>

#include "data_store/client.hpp"
#include "cell_state.hpp"
#include "nmea_parser.hpp"
#include "at_parser.hpp"
#include "serial_channel.hpp"
#include "sms_receiver.hpp"

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
            /// PDP context we provision + read back. The modem holds several (the
            /// eSIM parks its own on a cid of its choosing); this says which one is
            /// OURS. Everything cid-scoped keys off it: AT+CGDCONT=<cid>,
            /// AT+CGCONTRDP=<cid> (source of cell.ip/DNS) and cell.apn.current.
            int          apn_cid = 1;
            std::string  rat;                           ///< "" leave; auto|gsm|lte|… (Sierra !SELRAT)
            unsigned     interval_sec = 30;
            bool         gps_enable = true;
            bool         sms_enable = false;            ///< MT SMS receive (off by default)
        };

        explicit CellularClient(Config cfg) : m_cfg(std::move(cfg)) {}
        ~CellularClient() override = default;

        /// Connect ds, read config overrides, open channels, run the reactor.
        /// Returns a process exit code.
        int run();

        /// Poll timer: query the modem + publish the current state. Also drives
        /// the MO-send data phase when fired with the send-timer act.
        int handle_timeout(const ACE_Time_Value&, const void*) override;
        /// Reactor wakeup from the sms.send.request ds watch (listener thread).
        int handle_exception(ACE_HANDLE = ACE_INVALID_HANDLE) override;

    private:
        void load_config_from_ds();
        /// Open the AT (+ optional GNSS) tty and re-arm every one-time setup
        /// step. False when the tty is not there — the caller keeps probing, so
        /// a modem switched on later re-attaches with no operator action.
        bool open_modem();
        void on_at_line(const std::string& line);
        void handle_at_line(const std::string& line);   ///< on_at_line minus queue bookkeeping
        void on_nmea_line(const std::string& line);
        void poll_modem();
        void publish();
        void publish_absent();   ///< clear live cell.* when the modem is gone
        void on_send_request(const data_store::Client::Event& ev);
        void start_send();          // reactor thread: encode + issue AT+CMGS
        void on_reset_request(const data_store::Client::Event& ev);
        void start_reset();         // reactor thread: CFUN radio cycle + re-arm setup
        void on_clear_sms_request(const data_store::Client::Event& ev);
        void start_clear_sms();     // reactor thread: wipe the received-SMS history

        /// Queue one AT command. NEVER write to the modem directly: the WP7702's
        /// AT parser silently DROPS a command that arrives before the previous
        /// one has answered, and the drop is nondeterministic. A burst of 10
        /// commands executes a random ~half. Polled commands survive (they are
        /// re-sent every tick); one-shot reads like ATI / AT+CGDCONT? / AT!SELRAT?
        /// are lost forever. See apps/docs/hw-bringup-wp7702-cellular-wan.md §6.4.
        void cmd(const std::string& c);
        void pump_cmdq();           ///< write the next command if none is in flight
        void cmd_done();            ///< a terminal response arrived → advance the queue

        Config                          m_cfg;
        data_store::Client              m_ds;
        CellularState                   m_state;
        GpsFix                          m_gps;
        std::unique_ptr<SerialChannel>  m_at;
        std::unique_ptr<SerialChannel>  m_gnss;
        bool                            m_apn_sent = false;
        /// AT tty torn down (modem switched off / unplugged). Set from the
        /// SerialChannel's close callback; the probe timer reaps the channel on
        /// the next tick — we must not delete the handler from inside its own
        /// handle_close.
        bool                            m_tty_lost = false;
        // Serialized AT command stream — see cmd(). One command in flight at a
        // time; the watchdog timer un-wedges the queue if a command never answers.
        std::deque<std::string>         m_cmdq;
        bool                            m_cmd_inflight = false;
        long                            m_cmd_timer = -1;   ///< watchdog timer id, -1 = none
        Reg                             m_lastReg = Reg::Unknown;
        Reg                             m_reg_cs = Reg::Unknown;   ///< +CREG (2G/3G CS)
        Reg                             m_reg_ps = Reg::Unknown;   ///< +CGREG (2G/3G PS)
        Reg                             m_reg_eps = Reg::Unknown;  ///< +CEREG (LTE EPS)
        bool                            m_rat_done = false;        ///< one-time Sierra !SELRAT apply/read
        bool                            m_ident_done = false;      ///< one-time ATI / AT+CNUM identity read
        /// Contexts collected from the current AT+CGDCONT? scan (one line each),
        /// cleared before the scan is issued → published as cell.apn.profiles.
        std::vector<PdpProfile>         m_pdp_scan;
        bool                            m_haveIp = false;

        // MO SMS send (sms.send.* envelope). The ds watch fires on the listener
        // thread → records the token + notify()s the reactor; start_send() runs
        // on the reactor thread, and the PDU data phase is a one-shot timer.
        std::mutex                      m_send_mtx;
        std::string                     m_send_token;              ///< baseline of sms.send.request
        bool                            m_send_pending = false;
        // Module restart (cell.reset.request envelope, same token idiom).
        // AT+CFUN=0/1 radio cycle — NOT AT!RESET: a full module reboot drops
        // the USB ttys for ~30s and the 10s/5-burst restart policy could
        // start-limit the daemon out.
        std::string                     m_reset_token;             ///< baseline of cell.reset.request
        bool                            m_reset_pending = false;
        std::string                     m_clear_token;             ///< baseline of sms.clear.request
        bool                            m_clear_pending = false;
        std::string                     m_send_pdu;                ///< encoded PDU awaiting the '>' data phase
        bool                            m_send_active = false;     ///< AT+CMGS issued, awaiting +CMGS/+CMS ERROR
        bool                            m_gps_via_at = false;  ///< GNSS over AT poll (no NMEA tty)
        Vendor                          m_vendor = Vendor::Generic;
        bool                            m_gps_started = false; ///< GNSS engine kicked
        unsigned                        m_poll_count = 0;      ///< for periodic GNSS restart
        SmsReceiver                     m_sms;                 ///< MT SMS receive state machine
        bool                            m_sms_setup = false;   ///< one-time CMGF/CNMI/CMGL done
};

} // namespace cellular

#endif /*__cellular_client_hpp__*/
