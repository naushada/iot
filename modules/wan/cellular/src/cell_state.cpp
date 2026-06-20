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

std::vector<KV> CellularState::to_kv() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::vector<KV> kv;
    if (m_haveCell) {
        if (!m_state.empty())    kv.push_back({"cell.state", m_state});
        if (!m_operator.empty()) kv.push_back({"cell.operator", m_operator});
        if (!m_tech.empty())     kv.push_back({"cell.tech", m_tech});
        if (!m_reg.empty())      kv.push_back({"cell.reg", m_reg});
        if (!m_ip.empty())       kv.push_back({"cell.ip", m_ip});
        if (!m_iccid.empty())    kv.push_back({"cell.iccid", m_iccid});
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
    return kv;
}

} // namespace cellular
