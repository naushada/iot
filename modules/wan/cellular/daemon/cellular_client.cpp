#include "cellular_client.hpp"

#include <cstdio>
#include <vector>

#include <ace/Log_Msg.h>
#include <ace/Reactor.h>
#include <ace/Time_Value.h>

#include "line_router.hpp"

namespace cellular {

namespace {
    bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
    // Distinct timer act so handle_timeout can tell the periodic poll (act=null)
    // from the one-shot MO-send data phase (act=&kSendTagStorage).
    const int   kSendTagStorage = 0;
    const void* const kSendAct = &kSendTagStorage;
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
    str("cell.rat",       m_cfg.rat);

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

    // MO SMS send: baseline the request token (so a stale value doesn't fire on
    // boot) then watch it. The callback runs on the ds listener thread → it only
    // records the token + notify()s the reactor (start_send runs reactor-side).
    {
        std::vector<data_store::Client::GetResult> got;
        if (m_ds.get({std::string("sms.send.request")}, got).ok && !got.empty() && got[0].has_value)
            if (auto s = data_store::to_string(got[0].value)) m_send_token = *s;
    }
    data_store::Client::WatchHandle wh = data_store::Client::kInvalidHandle;
    m_ds.watch(std::vector<std::string>{"sms.send.request"},
               [this](const data_store::Client::Event& ev) { on_send_request(ev); }, &wh);

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

int CellularClient::handle_timeout(const ACE_Time_Value&, const void* act) {
    if (act == kSendAct) {
        // MO-send data phase: the modem has had a moment to emit the '>' prompt
        // after AT+CMGS=<len>; now send the PDU + Ctrl-Z (0x1A). The +CMGS: /
        // +CMS ERROR result is handled in on_at_line.
        if (m_at && !m_send_pdu.empty()) m_at->write_raw(m_send_pdu + "\x1a");
        m_send_pdu.clear();
        return 0;
    }
    poll_modem();
    publish();
    return 0;
}

void CellularClient::on_send_request(const data_store::Client::Event& ev) {
    if (ev.key != "sms.send.request") return;
    const std::string val = data_store::to_string(ev.value).value_or(std::string());
    {
        std::lock_guard<std::mutex> lk(m_send_mtx);
        if (val.empty() || val == m_send_token) return;   // unchanged / cleared
        m_send_token = val;
        m_send_pending = true;
    }
    ACE_Reactor::instance()->notify(this);                 // → handle_exception
}

int CellularClient::handle_exception(ACE_HANDLE) {
    bool pending;
    { std::lock_guard<std::mutex> lk(m_send_mtx); pending = m_send_pending; m_send_pending = false; }
    if (pending) start_send();
    return 0;
}

void CellularClient::start_send() {
    if (!m_at) return;
    auto get = [this](const char* key) -> std::string {
        std::vector<data_store::Client::GetResult> got;
        if (m_ds.get({std::string(key)}, got).ok && !got.empty() && got[0].has_value)
            if (auto s = data_store::to_string(got[0].value)) return *s;
        return {};
    };
    const std::string to   = get("sms.send.to");
    const std::string text = get("sms.send.text");

    std::string pdu;
    int len = 0;
    if (to.empty() || !encode_sms_submit(to, text, pdu, len)) {
        m_ds.set_volatile(std::string("sms.send.status"),
                          data_store::Value{std::string("failed: invalid recipient")});
        ACE_ERROR((LM_ERROR, ACE_TEXT("%D [cell] SMS send: bad recipient '%C'\n"), to.c_str()));
        return;
    }
    m_ds.set_volatile(std::string("sms.send.status"), data_store::Value{std::string("sending")});
    m_at->write_line("AT+CMGF=0");                       // ensure PDU mode
    m_at->write_line("AT+CMGS=" + std::to_string(len));  // → modem emits '>'
    m_send_pdu = pdu;                                    // data phase on the one-shot timer
    ACE_Reactor::instance()->schedule_timer(this, kSendAct, ACE_Time_Value(1));
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [cell] SMS send → %C (%d octets)\n"), to.c_str(), len));
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
        // then drain anything already stored (PDU stat 4 = ALL). We do NOT force
        // AT+CPMS to a specific store — HW check showed the WP7702 only offers
        // "SM" (SIM), and forcing "ME" would misdirect reads; the modem's default
        // keeps the receive/read stores aligned and +CMTI carries the store name
        // + index anyway. The +CMGL/+CMTI/+CMGR replies land in on_at_line → m_sms.
        // See apps/docs/tdd-mangoh-cellular-sms.md.
        m_at->write_line("AT+CMGF=0");
        m_at->write_line("AT+CNMI=2,1,0,0,0");
        m_at->write_line("AT+CMGL=4");
        m_sms_setup = true;
        ACE_DEBUG((LM_INFO, ACE_TEXT("%D [cell] SMS receive enabled (PDU mode)\n")));
    }
    // Sierra RAT (radio-access-tech) select — one-time once the vendor is known.
    // Optionally force the operator's cell.rat (AT!SELRAT + a CFUN cycle so it
    // takes effect), then always read it back for cell.rat.current. This is the
    // knob that unwedges an LTE-only modem where only 2G is in range.
    if (m_vendor == Vendor::Sierra && !m_rat_done) {
        m_at->write_line("AT!ENTERCND=\"A710\"");
        const int idx = m_cfg.rat.empty() ? -1 : selrat_index(m_cfg.rat);
        if (idx >= 0) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "AT!SELRAT=%02d", idx);
            m_at->write_line(buf);
            m_at->write_line("AT+CFUN=0");
            m_at->write_line("AT+CFUN=1");
            ACE_DEBUG((LM_INFO, ACE_TEXT("%D [cell] RAT set to %C (!SELRAT=%d)\n"),
                       m_cfg.rat.c_str(), idx));
        } else if (!m_cfg.rat.empty()) {
            ACE_ERROR((LM_WARNING, ACE_TEXT("%D [cell] unknown cell.rat '%C' — "
                       "leaving RAT unchanged\n"), m_cfg.rat.c_str()));
        }
        m_at->write_line("AT!SELRAT?");   // → parse_selrat → cell.rat.current
        m_rat_done = true;
    }

    // One-time identity read: ATI (manufacturer/model/revision/IMEI in one
    // labelled block) + AT+CNUM (SIM number, often blank). Parsed in on_at_line.
    if (!m_ident_done) {
        m_at->write_line("ATI");
        m_at->write_line("AT+CNUM");
        m_at->write_line("AT+CGDCONT?");   // → cell.apn.current (provisioned data APN)
        m_ident_done = true;
    }

    m_at->write_line("AT+CSQ");
    m_at->write_line("AT+COPS?");
    // Poll every registration domain — a modem camped on 2G registers via +CREG
    // while +CEREG (LTE) reads not-registered; on_at_line combines them.
    m_at->write_line("AT+CREG?");
    m_at->write_line("AT+CGREG?");
    m_at->write_line("AT+CEREG?");
    // When not attached, ask the network reject cause (best-effort; some
    // firmwares answer ERROR, which parse_ceer simply ignores).
    if (m_lastReg == Reg::Denied || m_lastReg == Reg::NotRegistered ||
        m_lastReg == Reg::Searching)
        m_at->write_line("AT+CEER");
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
        // Update the matching domain, then combine to the best across all three
        // (dispatch_at_line already set cell.reg from this single line; override
        // it with the combined result so a 2G +CREG isn't masked by an LTE
        // +CEREG "not-registered").
        const Reg r = parse_creg(line);
        if      (starts_with(line, "+CREG:"))  m_reg_cs  = r;
        else if (starts_with(line, "+CGREG:")) m_reg_ps  = r;
        else                                   m_reg_eps = r;
        m_lastReg = best_reg(m_reg_cs, m_reg_ps, m_reg_eps);
        m_state.set_reg(m_lastReg);
        if (m_lastReg == Reg::Home || m_lastReg == Reg::Roaming)
            m_state.set_reg_reason("");        // registered → clear any stale cause
    } else if (starts_with(line, "!SELRAT:")) {
        m_state.set_rat(parse_selrat(line));   // → cell.rat.current
    } else if (starts_with(line, "+CEER:")) {
        if (m_lastReg != Reg::Home && m_lastReg != Reg::Roaming)
            m_state.set_reg_reason(parse_ceer(line));
    } else if (starts_with(line, "+CNUM:")) {
        m_state.set_msisdn(parse_cnum(line));
    } else if (starts_with(line, "+CGDCONT:")) {
        m_state.set_apn(parse_cgdcont(line));   // → cell.apn.current
    } else if (starts_with(line, "+CMGS:")) {
        m_ds.set_volatile(std::string("sms.send.status"),
                          data_store::Value{std::string("sent")});
        ACE_DEBUG((LM_INFO, ACE_TEXT("%D [cell] SMS sent: %C\n"), line.c_str()));
    } else if (starts_with(line, "+CMS ERROR") || starts_with(line, "+CME ERROR")) {
        // Only treat as a send failure while a send is in flight (these can also
        // answer a failed read); harmless to publish the last error otherwise.
        m_ds.set_volatile(std::string("sms.send.status"),
                          data_store::Value{std::string("failed: ") + line});
    } else if (!parse_imei(line).empty()) {
        m_state.set_imei(parse_imei(line));    // ATI "IMEI: ..." line
    } else if (!parse_labeled(line, "Model").empty()) {
        const std::string model = parse_labeled(line, "Model");
        m_state.set_model(model);
        m_state.set_capability(model_capability(model));
    } else if (!parse_labeled(line, "Revision").empty()) {
        m_state.set_fw(parse_labeled(line, "Revision"));
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
