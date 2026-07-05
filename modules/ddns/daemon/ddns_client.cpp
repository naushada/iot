#include "ddns_client.hpp"

#include <vector>

#include <ace/Log_Msg.h>
#include <ace/Reactor.h>
#include <ace/Time_Value.h>
#include <ace/OS_NS_time.h>

#include "data_store/value.hpp"

namespace ddns {

const char* to_string(State s) {
    switch (s) {
        case State::Disabled:      return "disabled";
        case State::WaitingClock:  return "waiting-clock";
        case State::Detecting:     return "detecting";
        case State::Updating:      return "updating";
        case State::Ok:            return "ok";
        case State::OkUnreachable: return "ok-unreachable";
        case State::Error:         return "error";
    }
    return "unknown";
}

namespace {
// The config keys we bulk-read and watch. net.iface.active.ip is watched so we
// react to reconnects/IP changes immediately instead of waiting a full interval.
const std::vector<std::string> kWatchKeys = {
    "ddns.enabled", "ddns.provider", "ddns.hostname", "ddns.interval",
    "ddns.refresh.force", "ddns.ip.source", "net.iface.active.ip",
};
} // namespace

void DdnsClient::load_config_from_ds() {
    std::vector<data_store::Client::GetResult> got;
    if (!m_ds.get(kWatchKeys, got).ok) return;
    for (const auto& r : got) {
        if (!r.has_value) continue;
        if (r.key == "ddns.enabled") {
            if (auto b = data_store::to_bool(r.value)) m_cfg.enabled = *b;
        } else if (r.key == "ddns.provider") {
            if (auto s = data_store::to_string(r.value)) m_cfg.provider = *s;
        } else if (r.key == "ddns.hostname") {
            if (auto s = data_store::to_string(r.value)) m_cfg.hostname = *s;
        } else if (r.key == "ddns.interval") {
            if (auto i = data_store::to_int32(r.value); i && *i > 0)
                m_cfg.interval_sec = static_cast<unsigned>(*i);
        } else if (r.key == "ddns.refresh.force") {
            if (auto i = data_store::to_int32(r.value); i && *i > 0)
                m_cfg.refresh_force_sec = static_cast<unsigned>(*i);
        } else if (r.key == "ddns.ip.source") {
            if (auto s = data_store::to_string(r.value)) m_cfg.ip_source = *s;
        }
    }
}

void DdnsClient::publish_state(State s) {
    m_state = s;
    ++m_version;
    m_ds.set(std::vector<data_store::KV>{
        {"ddns.state",   data_store::Value{std::string(to_string(s))}},
        {"ddns.version", data_store::Value{static_cast<std::int32_t>(m_version)}},
    });
}

void DdnsClient::set_error(const std::string& msg) {
    m_ds.set("ddns.last.error", data_store::Value{msg});
    publish_state(State::Error);
    ACE_ERROR((LM_ERROR, ACE_TEXT("%D [ddns] %C\n"), msg.c_str()));
}

int DdnsClient::run() {
    if (!m_ds.connect(m_cfg.ds_sock).ok) {
        ACE_ERROR_RETURN((LM_ERROR, ACE_TEXT("%D [ddns] ds connect failed\n")), 1);
    }
    load_config_from_ds();

    // Watch config + WAN IP. The callback runs on the ds listener thread → it
    // only flags dirty + notify()s the reactor; reload() runs reactor-side.
    m_ds.watch(kWatchKeys,
               [this](const data_store::Client::Event& ev) { on_config_event(ev); });

    ACE_DEBUG((LM_INFO,
        ACE_TEXT("%D [ddns] start: enabled=%d provider=%C host=%C interval=%us src=%C\n"),
        m_cfg.enabled ? 1 : 0, m_cfg.provider.c_str(),
        m_cfg.hostname.empty() ? "(unset)" : m_cfg.hostname.c_str(),
        m_cfg.interval_sec, m_cfg.ip_source.c_str()));

    publish_state(m_cfg.enabled ? State::Detecting : State::Disabled);

    // First tick after 1s, then every interval.
    ACE_Reactor::instance()->schedule_timer(
        this, nullptr, ACE_Time_Value(1),
        ACE_Time_Value(static_cast<time_t>(m_cfg.interval_sec)));

    ACE_Reactor::instance()->run_reactor_event_loop();
    return 0;
}

int DdnsClient::handle_timeout(const ACE_Time_Value&, const void*) {
    if (!m_cfg.enabled) {
        if (m_state != State::Disabled) publish_state(State::Disabled);
        return 0;
    }
    if (m_cfg.hostname.empty()) {
        set_error("ddns.hostname is unset");
        return 0;
    }

    // PR-1 skeleton: detection (FR-3/#515) and the provider update
    // (FR-4..FR-7/#516..#519) land in later PRs. For now, log the resolved
    // target so the daemon is observable end-to-end and publish "detecting".
    publish_state(State::Detecting);
    ACE_DEBUG((LM_INFO,
        ACE_TEXT("%D [ddns] tick: would detect public IP + update %C via %C "
                 "(not yet implemented — PR-2/PR-3)\n"),
        m_cfg.hostname.c_str(), m_cfg.provider.c_str()));
    return 0;
}

void DdnsClient::on_config_event(const data_store::Client::Event&) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_cfg_dirty = true;
    ACE_Reactor::instance()->notify(this);   // → handle_exception (reactor thread)
}

int DdnsClient::handle_exception(ACE_HANDLE) {
    bool dirty;
    { std::lock_guard<std::mutex> lk(m_mtx); dirty = m_cfg_dirty; m_cfg_dirty = false; }
    if (dirty) reload();
    return 0;
}

void DdnsClient::reload() {
    unsigned prev_interval = m_cfg.interval_sec;
    bool     prev_enabled  = m_cfg.enabled;
    load_config_from_ds();

    ACE_DEBUG((LM_INFO,
        ACE_TEXT("%D [ddns] config reload: enabled=%d provider=%C host=%C interval=%us\n"),
        m_cfg.enabled ? 1 : 0, m_cfg.provider.c_str(),
        m_cfg.hostname.empty() ? "(unset)" : m_cfg.hostname.c_str(),
        m_cfg.interval_sec));

    if (m_cfg.interval_sec != prev_interval) {
        ACE_Reactor::instance()->cancel_timer(this);
        ACE_Reactor::instance()->schedule_timer(
            this, nullptr, ACE_Time_Value(1),
            ACE_Time_Value(static_cast<time_t>(m_cfg.interval_sec)));
    }
    if (!m_cfg.enabled) {
        publish_state(State::Disabled);
    } else if (prev_enabled != m_cfg.enabled) {
        publish_state(State::Detecting);   // just turned on → detect on next tick
    }
}

} // namespace ddns
