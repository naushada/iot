#include "sms_receiver.hpp"

#include <cctype>
#include <cstdlib>
#include <utility>

namespace cellular {

namespace {

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

/// The last run of digits in `s` as an int, or -1 (e.g. +CMTI: "SM",3 → 3).
int last_int(const std::string& s) {
    std::size_t end = std::string::npos;
    for (std::size_t k = s.size(); k-- > 0; ) {
        if (is_digit(s[k])) { end = k; break; }
    }
    if (end == std::string::npos) return -1;
    std::size_t start = end;
    while (start > 0 && is_digit(s[start - 1])) --start;
    return std::atoi(s.substr(start, end - start + 1).c_str());
}

/// The first integer after the ':' in `s`, or -1 (e.g. +CMGL: 2,1,,23 → 2).
int first_int_after_colon(const std::string& s) {
    const auto colon = s.find(':');
    std::size_t k = (colon == std::string::npos) ? 0 : colon + 1;
    while (k < s.size() && !is_digit(s[k])) ++k;
    if (k >= s.size()) return -1;
    const std::size_t start = k;
    while (k < s.size() && is_digit(s[k])) ++k;
    return std::atoi(s.substr(start, k - start).c_str());
}

} // namespace

bool SmsReassembler::add(const SmsMessage& part, SmsMessage& full) {
    if (part.total <= 1 || part.part < 1 || part.part > part.total) return false;

    const std::string key =
        part.sender + "|" + std::to_string(part.ref) + "|" + std::to_string(part.total);

    // Bounded: never let stuck partials grow without limit.
    if (m_groups.find(key) == m_groups.end() && m_groups.size() >= kMaxGroups) {
        m_groups.erase(m_groups.begin());
    }

    Group& g = m_groups[key];
    if (g.total == 0) {                                 // first part seen
        g.total  = part.total;
        g.sender = part.sender;
        g.parts.assign(static_cast<std::size_t>(part.total), std::string());
        g.seen.assign(static_cast<std::size_t>(part.total), false);
    }
    const std::size_t i = static_cast<std::size_t>(part.part - 1);
    if (!g.seen[i]) { g.seen[i] = true; g.parts[i] = part.text; ++g.have; }
    if (part.part == 1) g.scts = part.scts;             // timestamp of the head

    if (g.have < g.total) return false;

    full = SmsMessage();
    full.sender = g.sender;
    full.scts   = g.scts;
    for (const auto& p : g.parts) full.text += p;
    m_groups.erase(key);
    return true;
}

bool SmsReceiver::wants(const std::string& line) const {
    if (m_st == St::AwaitPdu) return true;              // the PDU line follows a header
    return starts_with(line, "+CMTI:") || starts_with(line, "+CMGR:") ||
           starts_with(line, "+CMGL:") || starts_with(line, "+CMT:");
}

SmsReceiver::Out SmsReceiver::on_line(const std::string& line) {
    Out out;

    if (m_st == St::AwaitPdu) {
        // This line is the hex PDU for the header we just saw.
        m_st = St::Idle;
        const int  idx = m_index;
        const bool del = m_delete;
        m_index = -1; m_delete = false;

        SmsMessage msg;
        if (decode_sms_deliver(line, msg)) {
            msg.index = idx;
            if (del && idx >= 0) out.commands.push_back("AT+CMGD=" + std::to_string(idx));
            finish(std::move(msg), out);
        }
        return out;
    }

    if (starts_with(line, "+CMTI:")) {
        // New-message URC: read it back, remembering the slot for the +CMGR hdr.
        const int idx = last_int(line);
        if (idx >= 0) {
            out.commands.push_back("AT+CMGR=" + std::to_string(idx));
            m_pending.push_back(idx);
        }
        return out;
    }

    if (starts_with(line, "+CMGR:")) {
        // Reply header to our AT+CMGR — its slot is the one we queued. Delete
        // after reading so storage can't silently fill and drop new MT messages.
        m_index = m_pending.empty() ? -1 : m_pending.front();
        if (!m_pending.empty()) m_pending.pop_front();
        m_delete = (m_index >= 0);
        m_st = St::AwaitPdu;
        return out;
    }

    if (starts_with(line, "+CMGL:")) {
        // Storage-listing entry (startup drain): the index is in the header. Do
        // not delete mid-listing — some modems renumber slots on delete.
        m_index  = first_int_after_colon(line);
        m_delete = false;
        m_st = St::AwaitPdu;
        return out;
    }

    if (starts_with(line, "+CMT:")) {
        // Direct-deliver (CNMI mode 2,2): no storage slot, nothing to delete.
        m_index  = -1;
        m_delete = false;
        m_st = St::AwaitPdu;
        return out;
    }

    return out;
}

void SmsReceiver::finish(SmsMessage msg, Out& out) {
    if (msg.total <= 1) {                               // single-part
        out.messages.push_back(std::move(msg));
        return;
    }
    SmsMessage full;
    if (m_reasm.add(msg, full)) out.messages.push_back(std::move(full));
}

} // namespace cellular
