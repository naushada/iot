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
    got.clear();
    if (m_ds.get({std::string("sms.enable")}, got).ok && !got.empty() && got[0].has_value)
        if (auto b = data_store::to_bool(got[0].value)) m_cfg.sms_enable = *b;
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

    // Identify the modem family up front so the first poll already uses the
    // right ICCID + GNSS commands. The reply (manufacturer / model line) is
    // classified in on_at_line.
    m_at->write_line("AT+GMI");
    m_at->write_line("AT+CGMM");

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
    ++m_poll_count;
    if (!m_apn_sent && !m_cfg.apn.empty()) {
        m_at->write_line("AT+CGDCONT=1,\"IP\",\"" + m_cfg.apn + "\"");
        m_apn_sent = true;
    }
    if (m_cfg.sms_enable && !m_sms_setup) {
        // One-time: PDU mode, route new-message notifications as +CMTI URCs,
        // prefer modem storage, then drain anything already stored (PDU stat 4 =
        // ALL). The +CMGL/+CMTI/+CMGR replies are handled in on_at_line via
        // m_sms. See apps/docs/tdd-mangoh-cellular-sms.md.
        m_at->write_line("AT+CMGF=0");
        m_at->write_line("AT+CNMI=2,1,0,0,0");
        m_at->write_line("AT+CPMS=\"ME\",\"ME\",\"ME\"");
        m_at->write_line("AT+CMGL=4");
        m_sms_setup = true;
        ACE_DEBUG((LM_INFO, ACE_TEXT("%D [cell] SMS receive enabled (PDU mode)\n")));
    }
    m_at->write_line("AT+CSQ");
    m_at->write_line("AT+COPS?");
    m_at->write_line("AT+CEREG?");
    m_at->write_line("AT+CGPADDR=1");
    m_at->write_line(iccid_command(m_vendor));   // QCCID/ICCID/CCID per vendor

    if (m_cfg.gps_enable) {
        // Kick the GNSS engine (vendor-specific), then keep it alive — Sierra's
        // standalone fix sessions expire (maxtime), so re-issue periodically
        // (~every 6 polls). Once started, NMEA streams on the GNSS tty, or for
        // Quectel-without-a-NMEA-port we poll +QGPSLOC below.
        if (!m_gps_started || (m_poll_count % 6 == 0)) {
            for (const auto& cmd : gps_start_commands(m_vendor)) {
                m_at->write_line(cmd);
            }
            m_gps_started = true;
        }
        if (m_gps_via_at) {
            if (m_vendor == Vendor::Quectel) {
                m_at->write_line("AT+QGPSLOC=2");   // +QGPSLOC parser handles the reply
            } else if (m_vendor == Vendor::Sierra) {
                // Sierra reports a fix only on the NMEA port, not a single-line
                // AT reply — so AT-only GPS isn't supported. Tell the operator
                // once to set cell.gps.tty to the WP's NMEA port.
                ACE_ERROR((LM_WARNING,
                    ACE_TEXT("%D [cell] Sierra GPS needs a NMEA port — set "
                             "cell.gps.tty (e.g. /dev/ttyUSB1)\n")));
            }
        }
    }
}

void CellularClient::on_at_line(const std::string& line) {
    // SMS-related lines (URCs, +CMGR/+CMGL headers, and the PDU line that
    // follows) go to the receive state machine instead of the status dispatcher;
    // it returns the follow-up commands to issue and any decoded messages.
    if (m_cfg.sms_enable && m_sms.wants(line)) {
        SmsReceiver::Out out = m_sms.on_line(line);
        for (const auto& cmd : out.commands) m_at->write_line(cmd);
        for (const auto& msg : out.messages) {
            m_state.set_sms(msg);
            ACE_DEBUG((LM_INFO, ACE_TEXT("%D [cell] SMS from %C: %C\n"),
                       msg.sender.c_str(), msg.text.c_str()));
        }
        if (!out.messages.empty()) publish();
        return;
    }

    dispatch_at_line(line, m_state);
    if (starts_with(line, "+CREG:") || starts_with(line, "+CGREG:") ||
        starts_with(line, "+CEREG:")) {
        m_lastReg = parse_creg(line);
    } else if (starts_with(line, "+CGPADDR:")) {
        m_haveIp = !parse_cgpaddr(line).empty();
    } else if (m_vendor == Vendor::Generic) {
        // Classify the modem from the AT+GMI / AT+CGMM reply (a bare
        // manufacturer/model line). Once known, the next poll uses the right
        // ICCID + GNSS commands.
        const Vendor v = parse_vendor(line);
        if (v != Vendor::Generic) {
            m_vendor = v;
            m_gps_started = false;   // re-kick GNSS with the correct command
            ACE_DEBUG((LM_INFO,
                ACE_TEXT("%D [cell] modem vendor detected: %C\n"), line.c_str()));
        }
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
