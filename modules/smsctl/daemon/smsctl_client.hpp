#ifndef __smsctl_client_hpp__
#define __smsctl_client_hpp__

#include <cstdint>
#include <mutex>
#include <string>

#include <ace/Event_Handler.h>

#include "data_store/client.hpp"
#include "smsctl/executor.hpp"
#include "smsctl/session.hpp"

/**
 * @file smsctl_client.hpp
 * @brief The iot-smsctld daemon: authenticated device control over SMS.
 *
 * Reactor daemon (cellular-client idiom). It never touches the modem: inbound
 * MT SMS arrives through the ds keys cellular-client publishes
 * (sms.version + sms.last.*), and replies go back out through the MO envelope
 * (sms.send.{to,text,request}). Privileged actions are delegated to root
 * `.path` units via trigger files under /run/iot — the daemon itself runs
 * unprivileged in group `iot`.
 *
 * The ds watch callback fires on the ds LISTENER thread, so it only sets a
 * dirty flag + notify()s the reactor; all real work happens on the reactor
 * thread in handle_exception(). See apps/docs/tdd-smsctl.md.
 */

namespace smsctl {

class SmsCtlClient : public ACE_Event_Handler {
    public:
        struct Config {
            std::string ds_sock;   ///< "" → ds default socket
        };

        explicit SmsCtlClient(Config cfg) : m_cfg(std::move(cfg)) {}
        ~SmsCtlClient() override = default;

        int run();

        /// 1s sweep: expire sessions + factory-reset nonces. No modem traffic.
        int handle_timeout(const ACE_Time_Value&, const void*) override;
        /// Reactor wake-up from a ds watch (config change or new SMS).
        int handle_exception(ACE_HANDLE = ACE_INVALID_HANDLE) override;

    private:
        void load_config_from_ds();
        void publish_state();
        void drain_inbound();
        /// Resolve a user from auth.users.admin.* / auth.users.accounts.
        bool lookup_account(const std::string& id, Account& out);

        Config              m_cfg;
        data_store::Client  m_ds;
        SessionStore        m_sessions;
        bool                m_enabled = false;

        // Dirty flags set on the ds listener thread, consumed on the reactor
        // thread. m_mtx guards them + the last-seen inbound tuple.
        std::mutex          m_mtx;
        bool                m_cfg_dirty = false;
        bool                m_sms_dirty = false;

        // Replay guard: baselined at startup so SMS already sitting in the SIM
        // store (drained by cellular-client on boot) can never execute.
        std::string         m_seen_sender;
        std::string         m_seen_text;
        std::string         m_seen_ts;

        std::uint64_t       m_handled = 0;   ///< commands executed (→ log/telemetry)
};

} // namespace smsctl

#endif /* __smsctl_client_hpp__ */
