#include "cellular_client.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

#include <sys/wait.h>

#include <ace/Log_Msg.h>
#include <ace/Reactor.h>
#include <ace/Time_Value.h>

#include "data_store/stats_publisher.hpp"

#include "line_router.hpp"

namespace cellular {

namespace {
    bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
    // Distinct timer acts so handle_timeout can tell the periodic poll (act=null)
    // from the one-shot MO-send data phase and the AT command watchdog.
    const int   kSendTagStorage = 0;
    const void* const kSendAct = &kSendTagStorage;
    const int   kCmdTagStorage = 0;
    const void* const kCmdAct = &kCmdTagStorage;
    // Modem-presence probe: fires while the AT tty is absent, so a modem that is
    // switched on later re-attaches by itself (no systemd restart, no operator).
    const int   kProbeTagStorage = 0;
    const void* const kProbeAct = &kProbeTagStorage;
    const long  kProbeIntervalSec = 10;

    /// A command is finished when the modem emits one of these. Everything else
    /// (echo, URCs, `+CSQ:` payloads, the `> ` PDU prompt) is intermediate.
    bool is_at_terminal(const std::string& l) {
        return l == "OK" || l == "ERROR" || l == "NO CARRIER" || l == "ABORTED" ||
               starts_with(l, "+CME ERROR") || starts_with(l, "+CMS ERROR");
    }

    /// Long enough to cover a 2G AT+CMGS round trip; only a wedged modem hits it.
    const long kCmdTimeoutSec = 15;
    /// Backstop: if the modem stops answering, stop queueing new poll batches
    /// rather than growing without bound.
    const std::size_t kCmdQueueMax = 64;
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
    if (m_ds.get({std::string("cell.apn.cid")}, got).ok && !got.empty() && got[0].has_value)
        if (auto n = data_store::to_int32(got[0].value)) if (*n > 0) m_cfg.apn_cid = *n;
    got.clear();
    if (m_ds.get({std::string("cell.gps.enable")}, got).ok && !got.empty() && got[0].has_value)
        if (auto b = data_store::to_bool(got[0].value)) m_cfg.gps_enable = *b;
    got.clear();
    if (m_ds.get({std::string("sms.enable")}, got).ok && !got.empty() && got[0].has_value)
        if (auto b = data_store::to_bool(got[0].value)) m_cfg.sms_enable = *b;

    str("cell.qmi.dev",   m_cfg.qmi_dev);
    str("cell.wan.iface", m_cfg.wan_iface);
    got.clear();
    if (m_ds.get({std::string("cell.data.enable")}, got).ok && !got.empty() && got[0].has_value)
        if (auto b = data_store::to_bool(got[0].value)) m_cfg.data_enable = *b;
}

int CellularClient::run() {
    if (!m_ds.connect(m_cfg.ds_sock).ok) {
        ACE_ERROR_RETURN((LM_ERROR, ACE_TEXT("%D [cell] ds connect failed\n")), 1);
    }
    load_config_from_ds();

    // Restore the received-SMS history so the device-ui table (WAN → Cellular)
    // survives a daemon restart — otherwise the first new SMS after a restart
    // would republish a one-element inbox and clobber the persisted history.
    {
        std::string inbox;
        std::uint64_t count = 0;
        std::vector<data_store::Client::GetResult> got;
        if (m_ds.get({std::string("sms.inbox")}, got).ok && !got.empty() && got[0].has_value)
            if (auto s = data_store::to_string(got[0].value)) inbox = *s;
        got.clear();
        if (m_ds.get({std::string("sms.count")}, got).ok && !got.empty() && got[0].has_value)
            if (auto s = data_store::to_string(got[0].value)) {
                try { count = std::stoull(*s); } catch (...) { count = 0; }
            }
        if (!inbox.empty()) m_state.seed_inbox(inbox, count);
    }

    // No dedicated NMEA tty → read GNSS over the AT channel (AT+QGPSLOC).
    m_gps_via_at = m_cfg.gps_enable && m_cfg.gps_tty.empty();

    // The modem may be absent at boot (switched off, unplugged, or a board with
    // no WP module at all). That is NOT a fatal error: publish the truth
    // (cell.state=absent, live keys cleared) and keep running — the probe timer
    // below re-attaches the moment the tty appears, with no operator action.
    //
    // This used to exit(2), and the unit carried
    // ConditionPathExistsGlob=/dev/ttyUSB*, so on a modem-less boot the daemon
    // never ran at all — and therefore never cleared cell.*. The device-ui then
    // rendered the LAST session's persisted operator/signal/IP as though the
    // modem were live. See publish()/publish_absent(): live telemetry is now
    // volatile so it can never outlive the daemon again.
    if (!open_modem()) {
        publish_absent();
        ACE_DEBUG((LM_WARNING,
            ACE_TEXT("%D [cell] AT tty %C not present — waiting for the modem "
                     "(probing every %ds)\n"),
            m_cfg.modem_tty.c_str(), static_cast<int>(kProbeIntervalSec)));
    }

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

    // Module restart request (device-ui button): same baseline-then-watch
    // token idiom as the MO-send envelope.
    {
        std::vector<data_store::Client::GetResult> got;
        if (m_ds.get({std::string("cell.reset.request")}, got).ok && !got.empty() && got[0].has_value)
            if (auto s = data_store::to_string(got[0].value)) m_reset_token = *s;
    }
    data_store::Client::WatchHandle wr = data_store::Client::kInvalidHandle;
    m_ds.watch(std::vector<std::string>{"cell.reset.request"},
               [this](const data_store::Client::Event& ev) { on_reset_request(ev); }, &wr);

    // device-ui "Clear" on the Received SMS table. A UI-only ds wipe would not
    // work: the daemon owns the inbox in memory and would republish it on the
    // next message. So the clear is a command, like every other one.
    {
        std::vector<data_store::Client::GetResult> got;
        if (m_ds.get({std::string("sms.clear.request")}, got).ok && !got.empty() && got[0].has_value)
            if (auto s = data_store::to_string(got[0].value)) m_clear_token = *s;
    }
    data_store::Client::WatchHandle wc = data_store::Client::kInvalidHandle;
    m_ds.watch(std::vector<std::string>{"sms.clear.request"},
               [this](const data_store::Client::Event& ev) { on_clear_sms_request(ev); }, &wc);

    // First poll after 1s, then every interval. The reactor dispatches the
    // serial handles + this timer on one thread — no polling loop.
    // poll_modem() no-ops while the modem is absent (m_at == nullptr).
    ACE_Reactor::instance()->schedule_timer(
        this, nullptr, ACE_Time_Value(1),
        ACE_Time_Value(static_cast<time_t>(m_cfg.interval_sec)));

    // Modem-presence probe: reaps a dead channel and re-attaches a modem that
    // was switched on after us. Cheap (one open() attempt) and it is what lets a
    // power-cycled WP7702 recover with no operator action.
    ACE_Reactor::instance()->schedule_timer(
        this, kProbeAct, ACE_Time_Value(kProbeIntervalSec),
        ACE_Time_Value(kProbeIntervalSec));

    ACE_DEBUG((LM_INFO,
        ACE_TEXT("%D [cell] up: AT=%C GNSS=%C apn=%C every %us\n"),
        m_cfg.modem_tty.c_str(),
        m_gnss ? m_cfg.gps_tty.c_str() : (m_gps_via_at ? "AT+QGPSLOC" : "(none)"),
        m_cfg.apn.empty() ? "(unset)" : m_cfg.apn.c_str(),
        m_cfg.interval_sec));

    // Services page: lifecycle + L22 resource telemetry. The reactor is pumped
    // below, so the stats timer rides it (run_reactor_thread=false).
    m_ds.set(std::string("services.cellular.state"),
             data_store::Value{std::string("running")});
    static data_store::StatsPublisher stats("services.cellular",
        [this](const std::vector<data_store::KV>& kv) { m_ds.set(kv); });
    stats.open(data_store::StatsPublisher::STATS_FLUSH_SEC, false);

    ACE_Reactor::instance()->run_reactor_event_loop();
    // The daemon no longer exits when the modem disappears — it publishes
    // cell.state="absent" and waits for it to come back (see open_modem() +
    // the probe timer). Exiting was what left the device-ui showing a phantom
    // "connected" modem: systemd could not restart us on a modem-less board
    // (ConditionPathExistsGlob), so nobody ever cleared the persisted cell.*.
    return 0;
}

void CellularClient::cmd(const std::string& c) {
    if (c.empty()) return;
    m_cmdq.push_back(c);
    pump_cmdq();
}

void CellularClient::pump_cmdq() {
    if (!m_at || m_cmd_inflight || m_cmdq.empty()) return;
    const std::string next = m_cmdq.front();
    m_cmdq.pop_front();
    m_at->write_line(next);
    m_cmd_inflight = true;
    m_cmd_timer = ACE_Reactor::instance()->schedule_timer(
        this, kCmdAct, ACE_Time_Value(kCmdTimeoutSec));
    // AT+CMGS is the one command with a data phase: the modem answers with a
    // bare "> " prompt (no CR/LF, so LineAssembler never surfaces it as a line)
    // and only then accepts the PDU. Arm the data-phase timer HERE, when the
    // command actually goes on the wire — arming it in start_send() raced the
    // queue and could fire before AT+CMGS had even been written.
    if (starts_with(next, "AT+CMGS=")) {
        ACE_Reactor::instance()->schedule_timer(this, kSendAct, ACE_Time_Value(1));
    }
}

void CellularClient::cmd_done() {
    if (!m_cmd_inflight) return;          // an unsolicited OK — nothing in flight
    m_cmd_inflight = false;
    if (m_cmd_timer != -1) {
        ACE_Reactor::instance()->cancel_timer(m_cmd_timer);
        m_cmd_timer = -1;
    }
    pump_cmdq();
}

int CellularClient::handle_timeout(const ACE_Time_Value&, const void* act) {
    if (act == kProbeAct) {
        // Reap a channel whose tty died (deferred out of handle_close, which we
        // must not delete the handler from), then try to re-attach.
        if (m_tty_lost) {
            m_at.reset();
            m_gnss.reset();
            m_tty_lost = false;
        }
        if (!m_at && open_modem()) {
            ACE_DEBUG((LM_INFO,
                ACE_TEXT("%D [cell] modem back on %C — re-attached\n"),
                m_cfg.modem_tty.c_str()));
        }
        return 0;
    }
    if (act == kCmdAct) {
        // The in-flight command never answered. Drop it and move on, or the
        // queue stalls forever and cell.* freezes.
        m_cmd_timer = -1;
        if (m_cmd_inflight) {
            m_cmd_inflight = false;
            ACE_ERROR((LM_WARNING,
                ACE_TEXT("%D [cell] AT command timed out after %ds; "
                         "advancing queue (%u pending)\n"),
                static_cast<int>(kCmdTimeoutSec),
                static_cast<unsigned>(m_cmdq.size())));
        }
        if (m_send_active) {
            // A send that never produced +CMGS / +CMS ERROR. The modem may be
            // sitting at the '>' prompt, in which case it would swallow every
            // subsequent command as message text — ESC (0x1B) cancels it.
            if (m_at) m_at->write_raw("\x1b");
            m_send_active = false;
            m_send_pdu.clear();
            m_ds.set_volatile(std::string("sms.send.status"),
                              data_store::Value{std::string("failed: modem timeout")});
            ACE_ERROR((LM_ERROR, ACE_TEXT("%D [cell] SMS send timed out\n")));
        }
        pump_cmdq();
        return 0;
    }
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
    supervise_data_call();   // no-op unless cell.data.enable
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

void CellularClient::on_reset_request(const data_store::Client::Event& ev) {
    if (ev.key != "cell.reset.request") return;
    const std::string val = data_store::to_string(ev.value).value_or(std::string());
    {
        std::lock_guard<std::mutex> lk(m_send_mtx);
        if (val.empty() || val == m_reset_token) return;   // unchanged / cleared
        m_reset_token = val;
        m_reset_pending = true;
    }
    ACE_Reactor::instance()->notify(this);                 // → handle_exception
}

void CellularClient::on_clear_sms_request(const data_store::Client::Event& ev) {
    if (ev.key != "sms.clear.request") return;
    const std::string val = data_store::to_string(ev.value).value_or(std::string());
    {
        std::lock_guard<std::mutex> lk(m_send_mtx);
        if (val.empty() || val == m_clear_token) return;   // unchanged / cleared
        m_clear_token = val;
        m_clear_pending = true;
    }
    ACE_Reactor::instance()->notify(this);                 // → handle_exception
}

void CellularClient::start_clear_sms() {
    m_state.clear_sms();
    publish();                       // republish sms.* as an empty inbox + count 0
    // Also empty the modem store, not just the ds record: otherwise the next
    // restart's AT+CMGL=4 drain re-reads the very messages we just cleared and
    // repopulates sms.inbox. Guarded on the AT channel + sms.enable; delflag=4
    // deletes ALL slots (see the drain in poll_modem for the delflag rationale).
    if (m_at && m_cfg.sms_enable) cmd("AT+CMGD=1,4");
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [cell] received-SMS history cleared\n")));
}

int CellularClient::handle_exception(ACE_HANDLE) {
    bool send_pending, reset_pending, clear_pending;
    {
        std::lock_guard<std::mutex> lk(m_send_mtx);
        send_pending  = m_send_pending;  m_send_pending  = false;
        reset_pending = m_reset_pending; m_reset_pending = false;
        clear_pending = m_clear_pending; m_clear_pending = false;
    }
    if (send_pending)  start_send();
    if (reset_pending) start_reset();
    if (clear_pending) start_clear_sms();
    return 0;
}

void CellularClient::start_reset() {
    if (!m_at) return;
    // Re-read config first: the operator (device-ui, or an `IOT APN …` SMS) may
    // have changed cell.apn / cell.rat since startup, and the whole point of the
    // cycle is to apply it — m_apn_sent/m_rat_done are re-armed below.
    load_config_from_ds();
    ACE_DEBUG((LM_INFO,
        ACE_TEXT("%D [cell] module restart requested — cycling radio (CFUN 0/1)\n")));
    cmd("AT+CFUN=0");
    cmd("AT+CFUN=1");
    // The radio cycle drops the data context, clears CNMI (SMS URC routing)
    // and re-opens the network search — re-arm every one-time setup so the
    // next polls re-apply APN / SMS / RAT / GNSS from scratch.
    m_apn_sent    = false;
    m_sms_setup   = false;
    m_rat_done    = false;
    m_gps_started = false;
    m_haveIp      = false;
    m_lastReg = m_reg_cs = m_reg_ps = m_reg_eps = Reg::Unknown;
    m_state.set_state("init");
    publish();
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
    m_send_pdu    = pdu;      // data phase is armed by pump_cmdq when AT+CMGS goes out
    m_send_active = true;     // → +CMGS / +CMS ERROR / watchdog all clear this
    cmd("AT+CMGF=0");                       // ensure PDU mode
    cmd("AT+CMGS=" + std::to_string(len));  // → modem emits '>'
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [cell] SMS send → %C (%d octets)\n"), to.c_str(), len));
}

bool CellularClient::open_modem() {
    auto at = std::unique_ptr<SerialChannel>(
        new SerialChannel([this](const std::string& l){ on_at_line(l); }));
    if (at->open(m_cfg.modem_tty, ACE_Reactor::instance()) == -1)
        return false;                       // not there (yet) — caller keeps probing

    m_at = std::move(at);

    // Modem unplugged / powered off → the AT tty tears down. Clear the now-stale
    // cell.* (otherwise the device-ui keeps showing the last operator/signal/ICCID
    // as if live) and mark the channel dead. We must NOT delete the SerialChannel
    // from inside its own handle_close — the probe timer reaps it on the next tick
    // and then re-attaches when the modem comes back.
    m_at->on_closed([this]() {
        ACE_ERROR((LM_WARNING,
            ACE_TEXT("%D [cell] modem tty closed (switched off / unplugged?) "
                     "— clearing cell.* and waiting for it to come back\n")));
        publish_absent();
        m_tty_lost = true;
    });

    if (m_cfg.gps_enable && !m_cfg.gps_tty.empty()) {
        m_gnss.reset(new SerialChannel([this](const std::string& l){ on_nmea_line(l); }));
        if (m_gnss->open(m_cfg.gps_tty, ACE_Reactor::instance()) == -1) {
            ACE_ERROR((LM_WARNING,
                ACE_TEXT("%D [cell] GNSS tty %C unavailable (continuing)\n"),
                m_cfg.gps_tty.c_str()));
            m_gnss.reset();
        }
    }

    // Re-arm every one-time step: a modem that just re-appeared (or was power
    // cycled) needs APN / SMS / RAT / identity / GNSS applied again from scratch.
    m_cmdq.clear();
    m_cmd_inflight = false;
    m_apn_sent = m_sms_setup = m_rat_done = m_ident_done = m_gps_started = false;
    m_vendor   = Vendor::Generic;
    m_haveIp   = false;
    m_lastReg  = m_reg_cs = m_reg_ps = m_reg_eps = Reg::Unknown;

    // A previous run may have died between AT+CMGS and its PDU, leaving the modem
    // parked at the '>' prompt — where it treats every following command as
    // message text. ESC (0x1B) cancels that. Harmless on a healthy modem.
    m_at->write_raw("\x1b");

    // Identify the modem family up front so the first poll already uses the
    // right ICCID + GNSS commands. The reply (manufacturer / model line) is
    // classified in on_at_line.
    cmd("AT+GMI");
    cmd("AT+CGMM");

    m_state.set_state("init");
    publish();
    return true;
}

void CellularClient::poll_modem() {
    if (!m_at) return;
    // The modem is not draining the queue (wedged, or slower than the poll
    // interval). Queueing another batch would only grow the backlog and make
    // cell.* increasingly stale; skip this tick instead.
    if (m_cmdq.size() > kCmdQueueMax) {
        ACE_ERROR((LM_WARNING,
            ACE_TEXT("%D [cell] AT queue backlogged (%u) — skipping poll\n"),
            static_cast<unsigned>(m_cmdq.size())));
        return;
    }
    ++m_poll_count;
    if (!m_apn_sent && !m_cfg.apn.empty()) {
        cmd("AT+CGDCONT=" + std::to_string(m_cfg.apn_cid) + ",\"IP\",\"" +
            m_cfg.apn + "\"");
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
        cmd("AT+CMGF=0");
        cmd("AT+CNMI=2,1,0,0,0");
        cmd("AT+CMGL=4");
        // The queue is serialized, so this runs after the drain completes:
        // delete ALL messages (delflag=4). The drain above has already copied
        // every stored message into the persistent sms.inbox record, so the SIM
        // copy is now redundant and must go — a SIM store that fills up silently
        // blocks MT-SMS delivery. delflag=1 ("delete READ") does NOT work here:
        // AT+CMGL=4 does not mark messages READ on the WP7702, so the just-drained
        // messages stay REC UNREAD and survive a READ-only delete, refilling the
        // inbox on every restart. The narrow window is a message landing between
        // the CMGL OK and this CMGD; that is far preferable to a store that never
        // empties, and genuinely-new messages arrive fresh via +CMTI afterwards.
        cmd("AT+CMGD=1,4");
        // Store usage → sms.storage ("used/total"): makes a full store
        // visible instead of just "SMS stopped arriving".
        cmd("AT+CPMS?");
        m_sms_setup = true;
        ACE_DEBUG((LM_INFO, ACE_TEXT("%D [cell] SMS receive enabled (PDU mode)\n")));
    }
    // Sierra RAT (radio-access-tech) select — one-time once the vendor is known.
    // Optionally force the operator's cell.rat (AT!SELRAT + a CFUN cycle so it
    // takes effect), then always read it back for cell.rat.current. This is the
    // knob that unwedges an LTE-only modem where only 2G is in range.
    if (m_vendor == Vendor::Sierra && !m_rat_done) {
        cmd("AT!ENTERCND=\"A710\"");
        const int idx = m_cfg.rat.empty() ? -1 : selrat_index(m_cfg.rat);
        if (idx >= 0) {
            char buf[16];
            std::snprintf(buf, sizeof buf, "AT!SELRAT=%02d", idx);
            cmd(buf);
            cmd("AT+CFUN=0");
            cmd("AT+CFUN=1");
            ACE_DEBUG((LM_INFO, ACE_TEXT("%D [cell] RAT set to %C (!SELRAT=%d)\n"),
                       m_cfg.rat.c_str(), idx));
        } else if (!m_cfg.rat.empty()) {
            ACE_ERROR((LM_WARNING, ACE_TEXT("%D [cell] unknown cell.rat '%C' — "
                       "leaving RAT unchanged\n"), m_cfg.rat.c_str()));
        }
        cmd("AT!SELRAT?");   // → parse_selrat → cell.rat.current
        m_rat_done = true;
    }

    // One-time identity read: ATI (manufacturer/model/revision/IMEI in one
    // labelled block) + AT+CNUM (SIM number, often blank). Parsed in on_at_line.
    if (!m_ident_done) {
        cmd("ATI");
        cmd("AT+CNUM");
        // → cell.apn.profiles (every context) + cell.apn.current (ours). Drop the
        // previous scan first so a context the operator deleted disappears from
        // the table instead of lingering.
        m_pdp_scan.clear();
        cmd("AT+CGDCONT?");
        m_ident_done = true;
    }

    cmd("AT+CSQ");
    cmd("AT+COPS?");
    // Poll every registration domain — a modem camped on 2G registers via +CREG
    // while +CEREG (LTE) reads not-registered; on_at_line combines them.
    cmd("AT+CREG?");
    cmd("AT+CGREG?");
    cmd("AT+CEREG?");
    // When not attached, ask the network reject cause (best-effort; some
    // firmwares answer ERROR, which parse_ceer simply ignores).
    if (m_lastReg == Reg::Denied || m_lastReg == Reg::NotRegistered ||
        m_lastReg == Reg::Searching)
        cmd("AT+CEER");
    cmd("AT+CGPADDR=1");
    // Dynamic context params → cell.dns. CGPADDR carries only a bare address, so
    // the carrier's resolvers are only obtainable here. Re-read every poll: the
    // bearer re-establishes on its own and the resolvers can change with it.
    cmd("AT+CGCONTRDP=" + std::to_string(m_cfg.apn_cid));
    cmd(iccid_command(m_vendor));   // QCCID/ICCID/CCID per vendor

    if (m_cfg.gps_enable) {
        // Kick the GNSS engine (vendor-specific), then keep it alive — Sierra's
        // standalone fix sessions expire (maxtime), so re-issue periodically
        // (~every 6 polls). Once started, NMEA streams on the GNSS tty, or for
        // Quectel-without-a-NMEA-port we poll +QGPSLOC below.
        if (!m_gps_started || (m_poll_count % 6 == 0)) {
            for (const auto& c : gps_start_commands(m_vendor)) {
                cmd(c);
            }
            m_gps_started = true;
        }
        if (m_gps_via_at) {
            if (m_vendor == Vendor::Quectel) {
                cmd("AT+QGPSLOC=2");   // +QGPSLOC parser handles the reply
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
    handle_at_line(line);
    // Advance the command queue only after the line has been consumed — a
    // terminal line can also carry meaning (+CME ERROR feeds sms.send.status).
    if (is_at_terminal(line)) cmd_done();
}

void CellularClient::handle_at_line(const std::string& line) {
    // SMS-related lines (URCs, +CMGR/+CMGL headers, and the PDU line that
    // follows) go to the receive state machine instead of the status dispatcher;
    // it returns the follow-up commands to issue and any decoded messages.
    if (m_cfg.sms_enable && m_sms.wants(line)) {
        SmsReceiver::Out out = m_sms.on_line(line);
        for (const auto& c : out.commands) cmd(c);
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
        m_state.set_reg_domains(reg_str(m_reg_cs), reg_str(m_reg_ps),
                                reg_str(m_reg_eps));
        if (m_lastReg == Reg::Home || m_lastReg == Reg::Roaming)
            m_state.set_reg_reason("");        // registered → clear any stale cause
    } else if (starts_with(line, "+CPMS:")) {
        m_state.set_sms_storage(parse_cpms(line));   // → sms.storage
    } else if (starts_with(line, "!SELRAT:")) {
        m_state.set_rat(parse_selrat(line));   // → cell.rat.current
    } else if (starts_with(line, "+CEER:")) {
        if (m_lastReg != Reg::Home && m_lastReg != Reg::Roaming)
            m_state.set_reg_reason(parse_ceer(line));
    } else if (starts_with(line, "+CNUM:")) {
        m_state.set_msisdn(parse_cnum(line));
    } else if (starts_with(line, "+CGDCONT:")) {
        // One line PER CONTEXT. Keep them all (cell.apn.profiles) and take
        // cell.apn.current from OUR cid only — the module carries the eSIM's
        // profile too, and folding every line into one APN let whichever context
        // happened to be listed last win.
        PdpProfile p;
        if (parse_cgdcont_entry(line, p)) {
            auto it = std::find_if(m_pdp_scan.begin(), m_pdp_scan.end(),
                                   [&p](const PdpProfile& e) { return e.cid == p.cid; });
            if (it == m_pdp_scan.end()) m_pdp_scan.push_back(p);
            else                        *it = p;
            std::sort(m_pdp_scan.begin(), m_pdp_scan.end(),
                      [](const PdpProfile& a, const PdpProfile& b) { return a.cid < b.cid; });
            m_state.set_apn_profiles(m_pdp_scan);
            if (p.cid == m_cfg.apn_cid) m_state.set_apn(p.apn);
        }
    } else if (starts_with(line, "+CMGS:")) {
        m_send_active = false;
        m_ds.set_volatile(std::string("sms.send.status"),
                          data_store::Value{std::string("sent")});
        ACE_DEBUG((LM_INFO, ACE_TEXT("%D [cell] SMS sent: %C\n"), line.c_str()));
    } else if (starts_with(line, "+CMS ERROR") || starts_with(line, "+CME ERROR")) {
        // Only a send failure while a send is in flight — these also answer any
        // failed read (AT+CEER, AT+CGDCONT= on an active context, …), and
        // clobbering sms.send.status with those made a successful send look bad.
        if (m_send_active) {
            m_send_active = false;
            m_send_pdu.clear();
            m_ds.set_volatile(std::string("sms.send.status"),
                              data_store::Value{std::string("failed: ") + line});
        }
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

void CellularClient::publish_absent() {
    // Explicitly blank the live cell.* keys — CellularState::to_kv only emits
    // non-empty fields, so it can't clear them; write the empties straight to ds.
    // sms.* and gps.* are left as last-known (they aren't "modem present" signals).
    static const char* kLiveKeys[] = {
        "cell.operator", "cell.tech", "cell.reg", "cell.signal.dbm",
        "cell.signal.bars", "cell.ip", "cell.dns", "cell.rat.current",
        "cell.reg.reason", "cell.iccid", "cell.imei", "cell.msisdn",
        "cell.model", "cell.fw", "cell.capability", "cell.apn.current",
        "cell.reg.cs", "cell.reg.ps", "cell.reg.eps",
    };
    std::vector<data_store::KV> batch;
    for (const char* k : kLiveKeys)
        batch.emplace_back(std::string(k), data_store::Value{std::string()});
    batch.emplace_back(std::string("cell.state"), data_store::Value{std::string("absent")});
    // PERSISTENT on purpose, even though publish() is volatile: an upgraded
    // device still carries the stale cell.* rows an older build wrote into
    // data_store.lua, and only a persistent write clears that on-disk layer (a
    // volatile write would sit on top of it and the phantom "connected" modem
    // would come back on the next ds restart). Idempotent afterwards.
    if (!m_ds.set(batch).ok)
        ACE_ERROR((LM_ERROR, ACE_TEXT("%D [cell] ds set(clear cell.*) failed\n")));
}

void CellularClient::publish() {
    // Derive the lifecycle token from registration + data context.
    //
    // With NO modem attached there is nothing to derive it FROM, and the answer
    // is not "init" — it is "absent". publish() runs on the poll timer whether or
    // not a modem is present, so without this guard it recomputed the token every
    // tick and overwrote the "absent" that publish_absent() had just set. The
    // live values were correctly cleared, but cell.state read "init" — so the
    // device-ui never showed its "Modem not detected" banner (it keys off
    // absent/sim-missing). Caught on HW.
    const char* tok = "absent";
    if (m_at) {
        tok = "init";
        if (m_haveIp)                                          tok = "connected";
        else if (m_lastReg == Reg::Home || m_lastReg == Reg::Roaming) tok = "registered";
        else if (m_lastReg == Reg::Searching)                 tok = "searching";
        else if (m_lastReg == Reg::Denied)                    tok = "failed";
    }
    m_state.set_state(tok);

    // cell.* / gps.* are LIVE TELEMETRY and are published VOLATILE — they must
    // not outlive the daemon that observes them. They used to be persistent, so
    // they were written to data_store.lua and survived a reboot: with the modem
    // switched off the daemon never ran, nothing cleared them, and the device-ui
    // happily rendered the previous session's operator/signal/IP as if the modem
    // were still connected. A volatile key cannot lie like that — it evaporates
    // with the ds-server, and the daemon republishes the truth (including
    // "absent") on every start.
    //
    // sms.* is the exception and stays PERSISTENT: the received-SMS history is a
    // record, not telemetry, and seed_inbox() deliberately restores it across a
    // daemon restart.
    std::vector<data_store::KV> volatile_batch;   // cell.* / gps.*
    std::vector<data_store::KV> persistent_batch; // sms.*
    for (const auto& e : m_state.to_kv()) {
        if (e.key.rfind("sms.", 0) == 0)
            persistent_batch.emplace_back(e.key, data_store::Value{e.value});
        else
            volatile_batch.emplace_back(e.key, data_store::Value{e.value});
    }
    if (!volatile_batch.empty() && !m_ds.set_volatile(volatile_batch).ok) {
        ACE_ERROR((LM_ERROR, ACE_TEXT("%D [cell] ds set(cell.*/gps.*) failed\n")));
    }
    if (!persistent_batch.empty() && !m_ds.set(persistent_batch).ok) {
        ACE_ERROR((LM_ERROR, ACE_TEXT("%D [cell] ds set(sms.*) failed\n")));
    }
}

std::string CellularClient::run_shell(const std::vector<std::string>& argv, int* rc) {
    // popen a shell-quoted argv with stderr merged (qmicli writes its "error:"
    // lines to stderr, and the parsers key off them). This BLOCKS the reactor for
    // the subprocess duration — fine for the sub-second status queries; the one
    // slow call is --wds-start-network (a few seconds on 2G), acceptable at the
    // 30s poll cadence. Mirrors net_router::shell::default_runner.
    if (argv.empty()) { if (rc) *rc = 127; return {}; }
    std::string cmdline;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i) cmdline += ' ';
        cmdline.push_back('\'');
        for (char c : argv[i]) { if (c == '\'') cmdline += "'\\''"; else cmdline.push_back(c); }
        cmdline.push_back('\'');
    }
    cmdline += " 2>&1";
    std::FILE* fp = ::popen(cmdline.c_str(), "r");
    if (!fp) { if (rc) *rc = 127; return {}; }
    std::string out;
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) out.append(buf, n);
    int status = ::pclose(fp);
    if (rc) *rc = WIFEXITED(status) ? WEXITSTATUS(status) : 127;
    return out;
}

void CellularClient::supervise_data_call() {
    if (!m_cfg.data_enable) return;   // ECM / status-only mode — nothing to do
    if (!m_at) return;                // modem absent; the probe timer re-attaches

    // The bearer needs the network. Gate on the combined registration the poll
    // already computed (Home/Roaming). Not registered → tear our view down and
    // wait; a re-registration on the next tick re-drives the bring-up.
    if (m_lastReg != Reg::Home && m_lastReg != Reg::Roaming) {
        if (m_data_up) { m_data_up = false; m_data_ip.clear(); }
        publish_data("waiting-reg");
        return;
    }

    // Already up? Confirm BOTH the QMI session ("connected") and that wwan0 still
    // carries the IP we assigned — a modem re-enumeration silently drops the WDS
    // session and wipes the netdev address, so neither alone is sufficient.
    const std::string status = run_shell(
        {"qmicli", "-d", m_cfg.qmi_dev, "-p", "--wds-get-packet-service-status"});
    const std::string addr = run_shell(
        {"ip", "-o", "-4", "addr", "show", "dev", m_cfg.wan_iface});
    const bool has_ip = !m_data_ip.empty() && addr.find(m_data_ip) != std::string::npos;
    if (parse_connected(status) && has_ip) {
        m_data_up = true;
        m_data_fail = 0;
        publish_data("up");
        return;
    }

    // Down (or just re-enumerated). The PS (packet-switched) domain must be
    // attached before --wds-start-network, and on the WP7702 the AT and QMI views
    // of PS desync — AT+CGATT=1 re-syncs it. Check the QMI (authoritative) view;
    // if detached, nudge over AT (async, idempotent) and retry next tick.
    const std::string ss = run_shell(
        {"qmicli", "-d", m_cfg.qmi_dev, "-p", "--nas-get-serving-system"});
    if (!parse_ps_attached(ss)) {
        cmd("AT+CGATT=1");
        m_data_up = false;
        publish_data("attaching");
        return;
    }

    if (bring_up_data_call()) {
        m_data_up = true;
        m_data_fail = 0;
    } else {
        m_data_up = false;
        ++m_data_fail;
    }
}

bool CellularClient::bring_up_data_call() {
    const std::string& dev = m_cfg.wan_iface;
    publish_data("starting");

    // raw-ip must be set with the link DOWN — the qmi_wwan default is 802.3, which
    // silently drops the modem's raw-ip downlink. See §4.4.
    run_shell({"ip", "link", "set", dev, "down"});
    run_shell({"sh", "-c", "echo Y > /sys/class/net/" + dev + "/qmi/raw_ip"});
    run_shell({"ip", "link", "set", dev, "up"});

    // The APN comes from the selected PROFILE/context, not the SIM slot. When the
    // operator set cell.apn, use it explicitly; otherwise start the modem's own
    // context cell.apn.cid (AT+CGDCONT=<cid>), whose stored APN the modem carries
    // in NV — e.g. a Sierra WP defaults context 1 to "iot.swir". This matches how
    // the AT side provisions the same cid. See §3 (two APN stores) / §4.4.
    const std::string net_arg =
        m_cfg.apn.empty()
            ? "--wds-start-network=profile=" + std::to_string(m_cfg.apn_cid)
            : "--wds-start-network=apn=" + m_cfg.apn + ",ip-type=4";
    int rc = 0;
    const std::string out = run_shell(
        {"qmicli", "-d", m_cfg.qmi_dev, "-p", net_arg,
         "--client-no-release-cid"}, &rc);
    const StartResult sr = parse_start_network(out);
    if (sr.status == StartResult::Status::CidTimeout) {
        // Leaked WDS CIDs across re-enumerations wedge the qmi-proxy — restart it
        // so the next tick allocates a fresh CID.
        run_shell({"killall", "qmi-proxy"});
        ACE_ERROR((LM_WARNING,
            ACE_TEXT("%D [cell] wds-start-network: qmi-proxy CID timeout — "
                     "killed proxy, retrying next tick\n")));
        publish_data("error");
        return false;
    }
    if (sr.status != StartResult::Status::Started) {
        ACE_ERROR((LM_WARNING,
            ACE_TEXT("%D [cell] wds-start-network failed (%C %C)\n"),
            sr.end_reason.c_str(), sr.verbose.c_str()));
        publish_data("error");
        return false;
    }

    const std::string cs = run_shell(
        {"qmicli", "-d", m_cfg.qmi_dev, "-p", "--wds-get-current-settings"});
    DirectIpSettings s = parse_current_settings(cs);
    if (!s.valid) {
        ACE_ERROR((LM_ERROR,
            ACE_TEXT("%D [cell] bearer up but current-settings had no IPv4\n")));
        publish_data("error");
        return false;
    }

    const int prefix = s.prefix > 0 ? s.prefix : 30;
    run_shell({"ip", "addr", "replace", s.ip + "/" + std::to_string(prefix),
               "dev", dev});
    // Install a default route carrying the gateway so net-router (cellular slot =
    // wwan0) can discover it (`ip route show default dev wwan0`) and re-metric it
    // by WAN priority. Metric 700 keeps cellular as the last-resort WAN until then.
    if (!s.gateway.empty())
        run_shell({"ip", "route", "replace", "default", "via", s.gateway,
                   "dev", dev, "metric", "700"});
    else
        run_shell({"ip", "route", "replace", "default", "dev", dev,
                   "metric", "700"});
    // Feed the carrier resolvers to this link (best-effort; Airtel blocks 1.1.1.1,
    // one of resolved's global fallbacks, so its own resolvers are more reliable).
    if (!s.dns.empty()) {
        std::vector<std::string> argv = {"resolvectl", "dns", dev};
        for (const auto& d : s.dns) argv.push_back(d);
        run_shell(argv);
    }

    m_data_ip = s.ip;
    publish_data("up", &s);
    ACE_DEBUG((LM_INFO,
        ACE_TEXT("%D [cell] DirectIP up on %C: %C/%d gw %C\n"),
        dev.c_str(), s.ip.c_str(), prefix,
        s.gateway.empty() ? "(none)" : s.gateway.c_str()));
    return true;
}

void CellularClient::publish_data(const char* state, const DirectIpSettings* s) {
    std::vector<data_store::KV> b;
    b.emplace_back(std::string("cell.data.state"), data_store::Value{std::string(state)});
    if (s) {
        b.emplace_back(std::string("cell.data.ip"),      data_store::Value{s->ip});
        b.emplace_back(std::string("cell.data.gateway"), data_store::Value{s->gateway});
        std::string dns;
        for (std::size_t i = 0; i < s->dns.size(); ++i) {
            if (i) dns += ",";
            dns += s->dns[i];
        }
        b.emplace_back(std::string("cell.data.dns"), data_store::Value{dns});
    }
    m_ds.set_volatile(b);
}

} // namespace cellular
