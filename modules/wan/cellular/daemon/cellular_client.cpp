#include "cellular_client.hpp"

#include <vector>

#include <ace/Log_Msg.h>
#include <ace/Reactor.h>
#include <ace/Time_Value.h>

#include "line_router.hpp"

namespace cellular {

namespace {
    bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
}

void CellularClient::load_config_from_ds() {
    auto str = [this](const char* key, std::string& dst) {
        std::vector<data_store::Client::GetResult> got;
        if (m_ds.get({std::string(key)}, got).ok && !got.empty() && got[0].has_value)
            if (auto s = data_store::to_string(got[0].value)) { if (!s->empty()) dst = *s; }
    };
    str("cell.modem.tty", m_cfg.modem_tty);
    str("cell.gps.tty",   m_cfg.gps_tty);
    str("cell.apn",       m_cfg.apn);

    std::vector<data_store::Client::GetResult> got;
    if (m_ds.get({std::string("cell.poll.interval.sec")}, got).ok && !got.empty() && got[0].has_value)
        if (auto n = data_store::to_int32(got[0].value)) if (*n > 0) m_cfg.interval_sec = static_cast<unsigned>(*n);
    got.clear();
    if (m_ds.get({std::string("cell.gps.enable")}, got).ok && !got.empty() && got[0].has_value)
        if (auto b = data_store::to_bool(got[0].value)) m_cfg.gps_enable = *b;
}

int CellularClient::run() {
    if (!m_ds.connect(m_cfg.ds_sock).ok) {
        ACE_ERROR_RETURN((LM_ERROR, ACE_TEXT("%D [cell] ds connect failed\n")), 1);
    }
    load_config_from_ds();
    // No dedicated NMEA tty → read GNSS over the AT channel (AT+QGPSLOC).
    m_gps_via_at = m_cfg.gps_enable && m_cfg.gps_tty.empty();

    m_at.reset(new SerialChannel([this](const std::string& l){ on_at_line(l); }));
    if (m_at->open(m_cfg.modem_tty, ACE_Reactor::instance()) == -1) {
        m_state.set_state("absent");
        publish();
        ACE_ERROR_RETURN((LM_ERROR,
            ACE_TEXT("%D [cell] AT tty %C unavailable\n"), m_cfg.modem_tty.c_str()), 2);
    }

    if (m_cfg.gps_enable && !m_cfg.gps_tty.empty()) {
        m_gnss.reset(new SerialChannel([this](const std::string& l){ on_nmea_line(l); }));
        if (m_gnss->open(m_cfg.gps_tty, ACE_Reactor::instance()) == -1) {
            ACE_ERROR((LM_WARNING,
                ACE_TEXT("%D [cell] GNSS tty %C unavailable (continuing)\n"),
                m_cfg.gps_tty.c_str()));
            m_gnss.reset();
        }
    }

    m_state.set_state("init");
    publish();

    // First poll after 1s, then every interval. The reactor dispatches the
    // serial handles + this timer on one thread — no polling loop.
    ACE_Reactor::instance()->schedule_timer(
        this, nullptr, ACE_Time_Value(1),
        ACE_Time_Value(static_cast<time_t>(m_cfg.interval_sec)));

    ACE_DEBUG((LM_INFO,
        ACE_TEXT("%D [cell] up: AT=%C GNSS=%C apn=%C every %us\n"),
        m_cfg.modem_tty.c_str(),
        m_gnss ? m_cfg.gps_tty.c_str() : (m_gps_via_at ? "AT+QGPSLOC" : "(none)"),
        m_cfg.apn.empty() ? "(unset)" : m_cfg.apn.c_str(),
        m_cfg.interval_sec));

    ACE_Reactor::instance()->run_reactor_event_loop();
    return 0;
}

int CellularClient::handle_timeout(const ACE_Time_Value&, const void*) {
    poll_modem();
    publish();
    return 0;
}

void CellularClient::poll_modem() {
    if (!m_at) return;
    if (!m_apn_sent && !m_cfg.apn.empty()) {
        m_at->write_line("AT+CGDCONT=1,\"IP\",\"" + m_cfg.apn + "\"");
        m_apn_sent = true;
    }
    m_at->write_line("AT+CSQ");
    m_at->write_line("AT+COPS?");
    m_at->write_line("AT+CEREG?");
    m_at->write_line("AT+CGPADDR=1");
    m_at->write_line("AT+QCCID");
    if (m_gps_via_at) {
        // Turn the GNSS engine on once, then query a fix each tick. A no-fix
        // reply (+CME ERROR 516) is simply ignored by the parser.
        if (!m_qgps_on) {
            m_at->write_line("AT+QGPS=1");
            m_qgps_on = true;
        }
        m_at->write_line("AT+QGPSLOC=2");
    }
}

void CellularClient::on_at_line(const std::string& line) {
    dispatch_at_line(line, m_state);
    if (starts_with(line, "+CREG:") || starts_with(line, "+CGREG:") ||
        starts_with(line, "+CEREG:")) {
        m_lastReg = parse_creg(line);
    } else if (starts_with(line, "+CGPADDR:")) {
        m_haveIp = !parse_cgpaddr(line).empty();
    }
}

void CellularClient::on_nmea_line(const std::string& line) {
    dispatch_nmea_line(line, m_gps, m_state);
}

void CellularClient::publish() {
    // Derive the lifecycle token from registration + data context.
    const char* tok = "init";
    if (m_haveIp)                                          tok = "connected";
    else if (m_lastReg == Reg::Home || m_lastReg == Reg::Roaming) tok = "registered";
    else if (m_lastReg == Reg::Searching)                 tok = "searching";
    else if (m_lastReg == Reg::Denied)                    tok = "failed";
    m_state.set_state(tok);

    std::vector<data_store::KV> batch;
    for (const auto& e : m_state.to_kv()) {
        batch.emplace_back(e.key, data_store::Value{e.value});
    }
    if (!batch.empty() && !m_ds.set(batch).ok) {
        ACE_ERROR((LM_ERROR, ACE_TEXT("%D [cell] ds set(cell.*/gps.*) failed\n")));
    }
}

} // namespace cellular
