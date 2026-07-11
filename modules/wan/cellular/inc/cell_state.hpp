#ifndef __cellular_cell_state_hpp__
#define __cellular_cell_state_hpp__

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "at_parser.hpp"
#include "nmea_parser.hpp"
#include "sms_pdu.hpp"

/**
 * @file cell_state.hpp
 * @brief Latest cellular + GPS state, and the ds key batch it publishes.
 *
 * Pure/ACE-free (host-testable), mirroring SensorCache: the daemon feeds it the
 * parsed AT/NMEA values and calls `to_kv()` to publish `cell.*` (modem status)
 * and `gps.*` (location) into the data-store. The lwm2m client mirrors `gps.*`
 * into LwM2M Object 6 (Location); the device-ui shows `cell.*` locally.
 */

namespace cellular {

struct KV {
    std::string key;
    std::string value;
};

class CellularState {
    public:
        /// Connection lifecycle token: "absent" / "init" / "sim-missing" /
        /// "searching" / "registered" / "connecting" / "connected" / "failed".
        void set_state(const std::string& token);
        void set_signal(const Signal& s);
        void set_operator(const Operator& op);
        void set_reg(Reg r);
        /// Per-domain registration (CS / PS / EPS). Diagnostics: MT-SMS on 2G
        /// rides the CS domain, which the combined cell.reg can mask (a PS
        /// "home" hides a CS "not-registered").
        void set_reg_domains(const std::string& cs, const std::string& ps,
                             const std::string& eps);
        /// Modem's current radio-access-tech selection (from AT!SELRAT?).
        void set_rat(const std::string& rat);
        /// Network reject/registration reason (from AT+CEER); "" clears it.
        void set_reg_reason(const std::string& reason);
        void set_ip(const std::string& ip);
        void set_iccid(const std::string& iccid);
        /// Modem/SIM identity (from ATI / AT+CNUM). Each ignores an empty value
        /// and is set at most once meaningfully; capability is model-derived.
        void set_imei(const std::string& imei);
        void set_msisdn(const std::string& msisdn);
        void set_model(const std::string& model);
        void set_fw(const std::string& fw);
        void set_capability(const std::string& cap);
        /// Provisioned data-context APN (from AT+CGDCONT?).
        void set_apn(const std::string& apn);
        /// Carrier DNS resolvers for the data context (from AT+CGCONTRDP=1), as a
        /// comma-joined list. Mirrors the `vpn.assigned.dns` convention.
        void set_dns(const std::string& dns);
        void set_gps(const GpsFix& fix);
        /// Record a received SMS: prepends to the inbox (bounded), updates
        /// sms.last.*, and bumps sms.count.
        void set_sms(const SmsMessage& msg);
        /// SIM message-store usage "used/total" (from AT+CPMS?) — a full
        /// store silently blocks MT-SMS delivery.
        void set_sms_storage(const std::string& usage);
        /// Restore the received-SMS history at startup from the persisted
        /// sms.inbox JSON array (newest first) + sms.count, so a daemon restart
        /// does not drop the table the device-ui shows. No-op on empty/corrupt.
        void seed_inbox(const std::string& inbox_json, std::uint64_t count);

        /// `cell.*` + `gps.*` batch (only populated fields are emitted), plus a
        /// per-domain version counter for the device-ui long-poll.
        std::vector<KV> to_kv() const;

    private:
        mutable std::mutex m_mtx;
        std::string m_state;
        std::string m_operator, m_tech, m_reg, m_ip, m_iccid, m_rat, m_regReason, m_dns;
        std::string m_regCs, m_regPs, m_regEps;
        std::string m_smsStorage;
        std::string m_imei, m_msisdn, m_model, m_fw, m_capability, m_apn;
        int  m_dbm = 0, m_bars = 0;
        bool m_haveSignal = false, m_haveCell = false;
        GpsFix m_gps;
        bool m_haveGps = false;
        std::string m_smsSender, m_smsText, m_smsTs;
        std::deque<SmsMessage> m_smsInbox;   ///< received-SMS history, newest first (bounded)
        std::uint64_t m_smsCount = 0;
        bool m_haveSms = false;
        std::uint64_t m_cellVersion = 0, m_gpsVersion = 0, m_smsVersion = 0;
};

} // namespace cellular

#endif /*__cellular_cell_state_hpp__*/
