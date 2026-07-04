#include "cell_state.hpp"

#include <cstdio>

namespace cellular {

namespace {
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
    m_smsSender = msg.sender;
    m_smsText   = msg.text;
    m_smsTs     = msg.scts;
    ++m_smsCount; m_haveSms = true; ++m_smsVersion;
}

std::vector<KV> CellularState::to_kv() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::vector<KV> kv;
    if (m_haveCell) {
        if (!m_state.empty())    kv.push_back({"cell.state", m_state});
        if (!m_operator.empty()) kv.push_back({"cell.operator", m_operator});
        if (!m_tech.empty())     kv.push_back({"cell.tech", m_tech});
        if (!m_reg.empty())      kv.push_back({"cell.reg", m_reg});
        if (!m_rat.empty())      kv.push_back({"cell.rat.current", m_rat});
        if (!m_regReason.empty())kv.push_back({"cell.reg.reason", m_regReason});
        if (!m_ip.empty())       kv.push_back({"cell.ip", m_ip});
        if (!m_iccid.empty())    kv.push_back({"cell.iccid", m_iccid});
        if (!m_imei.empty())     kv.push_back({"cell.imei", m_imei});
        if (!m_msisdn.empty())   kv.push_back({"cell.msisdn", m_msisdn});
        if (!m_model.empty())    kv.push_back({"cell.model", m_model});
        if (!m_fw.empty())       kv.push_back({"cell.fw", m_fw});
        if (!m_capability.empty())kv.push_back({"cell.capability", m_capability});
        if (!m_apn.empty())      kv.push_back({"cell.apn.current", m_apn});
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
        kv.push_back({"sms.version", std::to_string(m_smsVersion)});
    }
    return kv;
}

} // namespace cellular
