#ifndef __cellular_sms_receiver_hpp__
#define __cellular_sms_receiver_hpp__

#include <deque>
#include <map>
#include <string>
#include <vector>

#include "sms_pdu.hpp"

/**
 * @file sms_receiver.hpp
 * @brief URC-driven mobile-terminated SMS receive state machine (pure).
 *
 * The modem, configured `AT+CMGF=0` (PDU) + `AT+CNMI=2,1,…`, signals a new
 * message with a `+CMTI: "SM",<i>` URC; the daemon must then read it
 * (`AT+CMGR=<i>`), decode the PDU, and clear storage (`AT+CMGD=<i>`). That is a
 * *stateful, command-issuing* flow the stateless status router (dispatch_at_line)
 * can't express, so it lives here. `SmsReceiver` consumes the SMS-related AT
 * lines and returns the follow-up commands to send plus any completed messages;
 * `SmsReassembler` stitches concatenated (multipart) messages back together.
 *
 * Pure/ACE-free and host-unit-testable: the daemon (PR-C) wires `Out.commands`
 * to SerialChannel::write_line and `Out.messages` to CellularState::set_sms.
 */

namespace cellular {

/// Reassembles the parts of a concatenated SMS, keyed by (sender, ref, total).
class SmsReassembler {
    public:
        /// Add one decoded part. Returns true and fills `full` (sender + the
        /// concatenated body) once every part of its group has arrived; false
        /// while still incomplete or if `part` is not a multipart fragment.
        bool add(const SmsMessage& part, SmsMessage& full);

    private:
        struct Group {
            int total = 0;
            int have  = 0;
            std::string sender;
            std::string scts;
            std::vector<std::string> parts;   ///< sized `total`, indexed part-1
            std::vector<bool>        seen;
        };
        static constexpr std::size_t kMaxGroups = 16;   ///< bounded — drop oldest
        std::map<std::string, Group> m_groups;
};

/// Consumes SMS-related AT lines and drives the receive flow.
class SmsReceiver {
    public:
        struct Out {
            std::vector<std::string> commands;   ///< AT+CMGR / AT+CMGD to issue
            std::vector<SmsMessage>  messages;   ///< completed (reassembled) msgs
        };

        /// True when `line` should be routed here rather than to the status
        /// dispatcher — i.e. it is an SMS URC/response, or the PDU line we are
        /// currently awaiting after a `+CMGR`/`+CMGL`/`+CMT` header.
        bool wants(const std::string& line) const;

        /// Feed one line; returns the commands to issue + any finished messages.
        Out on_line(const std::string& line);

    private:
        enum class St { Idle, AwaitPdu };
        void finish(SmsMessage msg, Out& out);

        St              m_st     = St::Idle;
        int             m_index  = -1;      ///< storage slot of the pending PDU
        bool            m_delete = false;   ///< issue AT+CMGD after decoding it
        std::deque<int> m_pending;          ///< AT+CMGR=<i> indices awaiting a header
        SmsReassembler  m_reasm;
};

} // namespace cellular

#endif /*__cellular_sms_receiver_hpp__*/
