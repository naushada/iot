#include "cell_state.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>

#include <nlohmann/json.hpp>

namespace cellular {

namespace {
    /// Cap on the received-SMS history kept in memory / published to sms.inbox.
    constexpr std::size_t kSmsInboxMax = 20;

    std::string fmt(double v, int decimals) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
        return buf;
    }
}

void CellularState::set_state(const std::string& token) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_state = token; m_haveCell = true; ++m_cellVersion;
}

void CellularState::set_signal(const Signal& s) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!s.valid) return;
    m_dbm = s.dbm; m_bars = s.bars; m_haveSignal = true; m_haveCell = true; ++m_cellVersion;
}

void CellularState::set_operator(const Operator& op) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (!op.valid) return;
    m_operator = op.name;
    if (!op.tech.empty()) m_tech = op.tech;
    m_haveCell = true; ++m_cellVersion;
}

void CellularState::set_reg(Reg r) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_reg = reg_str(r); m_haveCell = true; ++m_cellVersion;
}

void CellularState::set_reg_domains(const std::string& cs,
                                    const std::string& ps,
                                    const std::string& eps) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (cs == m_regCs && ps == m_regPs && eps == m_regEps) return;
    m_regCs = cs; m_regPs = ps; m_regEps = eps;
    m_haveCell = true; ++m_cellVersion;
}

void CellularState::set_rat(const std::string& rat) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (rat.empty() || rat == m_rat) return;
    m_rat = rat; m_haveCell = true; ++m_cellVersion;
}

void CellularState::set_reg_reason(const std::string& reason) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (reason == m_regReason) return;
    m_regReason = reason; m_haveCell = true; ++m_cellVersion;
}

void CellularState::set_imei(const std::string& imei) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (imei.empty() || imei == m_imei) return;
    m_imei = imei; m_haveCell = true; ++m_cellVersion;
}

void CellularState::set_msisdn(const std::string& msisdn) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (msisdn.empty() || msisdn == m_msisdn) return;
    m_msisdn = msisdn; m_haveCell = true; ++m_cellVersion;
}

void CellularState::set_model(const std::string& model) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (model.empty() || model == m_model) return;
    m_model = model; m_haveCell = true; ++m_cellVersion;
}

void CellularState::set_fw(const std::string& fw) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (fw.empty() || fw == m_fw) return;
    m_fw = fw; m_haveCell = true; ++m_cellVersion;
}

void CellularState::set_capability(const std::string& cap) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (cap.empty() || cap == m_capability) return;
    m_capability = cap; m_haveCell = true; ++m_cellVersion;
}

void CellularState::set_apn(const std::string& apn) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (apn.empty() || apn == m_apn) return;
    m_apn = apn; m_haveCell = true; ++m_cellVersion;
}

void CellularState::set_apn_profiles(const std::vector<PdpProfile>& profiles) {
    std::lock_guard<std::mutex> lk(m_mtx);
    const bool same =
        m_pdpProfiles.size() == profiles.size() &&
        std::equal(m_pdpProfiles.begin(), m_pdpProfiles.end(), profiles.begin(),
                   [](const PdpProfile& a, const PdpProfile& b) {
                       return a.cid == b.cid && a.type == b.type && a.apn == b.apn;
                   });
    if (same) return;                 // the table is re-read on every scan
    m_pdpProfiles = profiles;
    m_haveCell = true; ++m_cellVersion;
}

void CellularState::set_dns(const std::string& dns) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (dns.empty() || dns == m_dns) return;
    m_dns = dns; m_haveCell = true; ++m_cellVersion;
}

void CellularState::set_ip(const std::string& ip) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_ip = ip; m_haveCell = true; ++m_cellVersion;
}

void CellularState::set_iccid(const std::string& iccid) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (iccid.empty()) return;
    m_iccid = iccid; m_haveCell = true; ++m_cellVersion;
}

void CellularState::set_gps(const GpsFix& fix) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_gps = fix; m_haveGps = true; ++m_gpsVersion;
}

void CellularState::set_sms(const SmsMessage& msg) {
    std::lock_guard<std::mutex> lk(m_mtx);
    // De-dup: a restart re-drains stored messages (AT+CMGL=4) and a slot that
    // wasn't deleted in time would otherwise land in the inbox a second time.
    // Same sender + timestamp + text is the same message — drop the repeat so
    // the history table never shows doubles (and sms.count isn't inflated).
    for (const auto& e : m_smsInbox)
        if (e.sender == msg.sender && e.scts == msg.scts && e.text == msg.text)
            return;
    m_smsSender = msg.sender;
    m_smsText   = msg.text;
    m_smsTs     = msg.scts;
    m_smsInbox.push_front(msg);                              // newest first
    while (m_smsInbox.size() > kSmsInboxMax) m_smsInbox.pop_back();
    ++m_smsCount; m_haveSms = true; ++m_smsVersion;
}

void CellularState::set_sms_storage(const std::string& usage) {
    std::lock_guard<std::mutex> lk(m_mtx);
    if (usage.empty() || usage == m_smsStorage) return;
    m_smsStorage = usage; m_haveCell = true; ++m_cellVersion;
}

void CellularState::clear_sms() {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_smsInbox.clear();
    m_smsCount = 0;
    m_smsSender.clear();
    m_smsText.clear();
    m_smsTs.clear();
    // m_haveSms stays TRUE on purpose: to_kv() only emits sms.* when it is set,
    // so clearing it would leave the old inbox/count stranded in ds forever.
    m_haveSms = true;
    ++m_smsVersion;
}

void CellularState::seed_inbox(const std::string& inbox_json, std::uint64_t count) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_smsCount = count;
    try {
        const auto arr = nlohmann::json::parse(inbox_json);
        if (arr.is_array()) {
            for (const auto& e : arr) {
                if (m_smsInbox.size() >= kSmsInboxMax) break;
                SmsMessage m;
                m.sender = e.value("from", std::string());
                m.text   = e.value("text", std::string());
                m.scts   = e.value("ts",   std::string());
                m_smsInbox.push_back(m);                     // array is already newest-first
            }
        }
    } catch (...) {
        // empty / corrupt persisted value → just start with an empty history
    }
    if (!m_smsInbox.empty()) {
        const auto& f = m_smsInbox.front();
        m_smsSender = f.sender; m_smsText = f.text; m_smsTs = f.scts;
    }
    // A corrupt inbox must still keep the running count alive — otherwise
    // a bad persisted value silently zeroes sms.count on restart.
    if (!m_smsInbox.empty() || m_smsCount > 0) m_haveSms = true;
}

std::vector<KV> CellularState::to_kv() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::vector<KV> kv;
    if (m_haveCell) {
        if (!m_state.empty())    kv.push_back({"cell.state", m_state});
        if (!m_operator.empty()) kv.push_back({"cell.operator", m_operator});
        if (!m_tech.empty())     kv.push_back({"cell.tech", m_tech});
        if (!m_reg.empty())      kv.push_back({"cell.reg", m_reg});
        if (!m_regCs.empty())    kv.push_back({"cell.reg.cs", m_regCs});
        if (!m_regPs.empty())    kv.push_back({"cell.reg.ps", m_regPs});
        if (!m_regEps.empty())   kv.push_back({"cell.reg.eps", m_regEps});
        if (!m_smsStorage.empty()) kv.push_back({"sms.storage", m_smsStorage});
        if (!m_rat.empty())      kv.push_back({"cell.rat.current", m_rat});
        if (!m_regReason.empty())kv.push_back({"cell.reg.reason", m_regReason});
        if (!m_ip.empty())       kv.push_back({"cell.ip", m_ip});
        if (!m_dns.empty())      kv.push_back({"cell.dns", m_dns});
        if (!m_iccid.empty())    kv.push_back({"cell.iccid", m_iccid});
        if (!m_imei.empty())     kv.push_back({"cell.imei", m_imei});
        if (!m_msisdn.empty())   kv.push_back({"cell.msisdn", m_msisdn});
        if (!m_model.empty())    kv.push_back({"cell.model", m_model});
        if (!m_fw.empty())       kv.push_back({"cell.fw", m_fw});
        if (!m_capability.empty())kv.push_back({"cell.capability", m_capability});
        if (!m_apn.empty())      kv.push_back({"cell.apn.current", m_apn});
        if (!m_pdpProfiles.empty()) {
            // Every provisioned PDP context, for the device-ui table. Same idiom
            // as sms.inbox: a JSON array, embedded parsed by the http-server.
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& p : m_pdpProfiles)
                arr.push_back({{"cid", p.cid}, {"type", p.type}, {"apn", p.apn}});
            kv.push_back({"cell.apn.profiles", arr.dump()});
        }
        if (m_haveSignal) {
            kv.push_back({"cell.signal.dbm",  std::to_string(m_dbm)});
            kv.push_back({"cell.signal.bars", std::to_string(m_bars)});
        }
        kv.push_back({"cell.version", std::to_string(m_cellVersion)});
    }
    if (m_haveGps) {
        kv.push_back({"gps.fix", m_gps.quality.empty() ? "none" : m_gps.quality});
        if (m_gps.valid) {
            kv.push_back({"gps.lat",    fmt(m_gps.lat, 6)});
            kv.push_back({"gps.lon",    fmt(m_gps.lon, 6)});
            kv.push_back({"gps.alt",    fmt(m_gps.alt_m, 1)});
            kv.push_back({"gps.speed",  fmt(m_gps.speed_kmh, 1)});
            kv.push_back({"gps.course", fmt(m_gps.course_deg, 1)});
            kv.push_back({"gps.sats",   std::to_string(m_gps.sats)});
            if (!m_gps.utc.empty()) kv.push_back({"gps.utc", m_gps.utc});
        }
        kv.push_back({"gps.version", std::to_string(m_gpsVersion)});
    }
    if (m_haveSms) {
        kv.push_back({"sms.last.sender", m_smsSender});
        kv.push_back({"sms.last.text",   m_smsText});
        if (!m_smsTs.empty()) kv.push_back({"sms.last.ts", m_smsTs});
        kv.push_back({"sms.count",   std::to_string(m_smsCount)});
        // Full received-SMS history as a JSON array (newest first) for the
        // device-ui table. nlohmann handles escaping of arbitrary SMS text.
        nlohmann::json inbox = nlohmann::json::array();
        for (const auto& m : m_smsInbox)
            inbox.push_back({{"ts", m.scts}, {"from", m.sender}, {"text", m.text}});
        kv.push_back({"sms.inbox", inbox.dump()});
        kv.push_back({"sms.version", std::to_string(m_smsVersion)});
    }
    return kv;
}

} // namespace cellular
