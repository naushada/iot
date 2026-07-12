#include "ddns_client.hpp"

#include "data_store/stats_publisher.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
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

// Config keys we bulk-read + watch. net.iface.active.ip is watched so we react
// to reconnects/IP changes immediately instead of waiting a full interval.
const std::vector<std::string> kConfigKeys = {
    "ddns.enabled", "ddns.provider", "ddns.hostname", "ddns.interval",
    "ddns.refresh.force", "ddns.ip.source", "ddns.token.path",
    "ddns.dyndns2.server", "ddns.dyndns2.user", "ddns.duckdns.domains",
    "ddns.cf.zone.id", "ddns.r53.zone.id", "ddns.r53.access.key",
};
// Secret keys (write-only: daemon reads them only when its gid matches the
// schema read_acl). Read but never logged.
const std::vector<std::string> kSecretKeys = {
    "ddns.dyndns2.token", "ddns.duckdns.token", "ddns.cf.token",
    "ddns.r53.secret.key",
};
// Watch = config + WAN IP (not the secrets — a secret change alone need not
// re-trigger; reload() re-reads everything anyway).
std::vector<std::string> watch_keys() {
    std::vector<std::string> k = kConfigKeys;
    k.push_back("net.iface.active.ip");
    return k;
}

long now_epoch() { return static_cast<long>(ACE_OS::time(nullptr)); }

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// FR-9: a credential-FILE override. Priority: ddns.token.path, else the systemd
// LoadCredential drop ($CREDENTIALS_DIRECTORY/ddns_secret). Returns "" if none.
std::string read_credential_file(const std::string& token_path) {
    std::string path = token_path;
    if (path.empty()) {
        if (const char* dir = std::getenv("CREDENTIALS_DIRECTORY"))
            path = std::string(dir) + "/ddns_secret";
    }
    if (path.empty()) return {};
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return trim(ss.str());
}

} // namespace

void DdnsClient::load_config_from_ds() {
    std::vector<std::string> keys = kConfigKeys;
    keys.insert(keys.end(), kSecretKeys.begin(), kSecretKeys.end());

    std::vector<data_store::Client::GetResult> got;
    if (!m_ds.get(keys, got).ok) return;
    for (const auto& r : got) {
        if (!r.has_value) continue;
        auto S = [&](std::string& dst){ if (auto s = data_store::to_string(r.value)) dst = *s; };
        if      (r.key == "ddns.enabled")        { if (auto b = data_store::to_bool(r.value)) m_cfg.enabled = *b; }
        else if (r.key == "ddns.provider")       S(m_cfg.provider);
        else if (r.key == "ddns.hostname")       S(m_cfg.hostname);
        else if (r.key == "ddns.interval")       { if (auto i = data_store::to_int32(r.value); i && *i > 0) m_cfg.interval_sec = static_cast<unsigned>(*i); }
        else if (r.key == "ddns.refresh.force")  { if (auto i = data_store::to_int32(r.value); i && *i > 0) m_cfg.refresh_force_sec = static_cast<unsigned>(*i); }
        else if (r.key == "ddns.ip.source")      S(m_cfg.ip_source);
        else if (r.key == "ddns.token.path")     S(m_cfg.token_path);
        else if (r.key == "ddns.dyndns2.server") S(m_cfg.dyndns2_server);
        else if (r.key == "ddns.dyndns2.user")   S(m_cfg.dyndns2_user);
        else if (r.key == "ddns.duckdns.domains")S(m_cfg.duckdns_domains);
        else if (r.key == "ddns.cf.zone.id")     S(m_cfg.cf_zone_id);
        else if (r.key == "ddns.r53.zone.id")    S(m_cfg.r53_zone_id);
        else if (r.key == "ddns.r53.access.key") S(m_cfg.r53_access_key);
        else if (r.key == "ddns.dyndns2.token")  S(m_cfg.dyndns2_token);
        else if (r.key == "ddns.duckdns.token")  S(m_cfg.duckdns_token);
        else if (r.key == "ddns.cf.token")       S(m_cfg.cf_token);
        else if (r.key == "ddns.r53.secret.key") S(m_cfg.r53_secret_key);
    }

    // Credential-file override for the ACTIVE provider's secret (FR-9).
    std::string cred = read_credential_file(m_cfg.token_path);
    if (!cred.empty()) {
        if      (m_cfg.provider == "dyndns2")    m_cfg.dyndns2_token  = cred;
        else if (m_cfg.provider == "duckdns")    m_cfg.duckdns_token  = cred;
        else if (m_cfg.provider == "cloudflare") m_cfg.cf_token       = cred;
        else if (m_cfg.provider == "route53")    m_cfg.r53_secret_key = cred;
    }
}

Creds DdnsClient::creds_for_provider() const {
    Creds c;
    if (m_cfg.provider == "dyndns2") {
        c.user = m_cfg.dyndns2_user; c.secret = m_cfg.dyndns2_token;
        c.target1 = m_cfg.dyndns2_server;
    } else if (m_cfg.provider == "duckdns") {
        c.secret = m_cfg.duckdns_token; c.target1 = m_cfg.duckdns_domains;
    } else if (m_cfg.provider == "cloudflare") {
        c.secret = m_cfg.cf_token; c.target1 = m_cfg.cf_zone_id;
    } else if (m_cfg.provider == "route53") {
        c.user = m_cfg.r53_access_key; c.secret = m_cfg.r53_secret_key;
        c.target1 = m_cfg.r53_zone_id;
    }
    return c;
}

// FR-11: heuristic reachability — the echo returns the outermost NAT IP as seen
// by the echo server. If that is a private / CGNAT / link-local address, the
// device sits behind carrier NAT and the name won't be directly reachable.
bool DdnsClient::looks_reachable(const std::string& ip) {
    unsigned a=0,b=0,c=0,d=0;
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a,&b,&c,&d) != 4) return false;
    if (a == 10) return false;                             // 10/8
    if (a == 127) return false;                            // loopback
    if (a == 0) return false;
    if (a == 172 && b >= 16 && b <= 31) return false;      // 172.16/12
    if (a == 192 && b == 168) return false;                // 192.168/16
    if (a == 169 && b == 254) return false;                // link-local
    if (a == 100 && b >= 64 && b <= 127) return false;     // 100.64/10 CGNAT
    return true;
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
    m_detector = make_echo_detector();

    m_ds.watch(watch_keys(),
               [this](const data_store::Client::Event& ev) { on_config_event(ev); });

    ACE_DEBUG((LM_INFO,
        ACE_TEXT("%D [ddns] start: enabled=%d provider=%C host=%C interval=%us src=%C\n"),
        m_cfg.enabled ? 1 : 0, m_cfg.provider.c_str(),
        m_cfg.hostname.empty() ? "(unset)" : m_cfg.hostname.c_str(),
        m_cfg.interval_sec, m_cfg.ip_source.c_str()));

    publish_state(m_cfg.enabled ? State::Detecting : State::Disabled);

    // Services page: lifecycle + L22 resource telemetry. The daemon already
    // pumps the singleton reactor below, so the stats timer rides it
    // (run_reactor_thread=false) — no extra thread.
    m_ds.set(std::string("services.ddns.state"),
             data_store::Value{std::string("running")});
    static data_store::StatsPublisher stats("services.ddns",
        [this](const std::vector<data_store::KV>& kv) { m_ds.set(kv); });
    stats.open(data_store::StatsPublisher::STATS_FLUSH_SEC, false);

    ACE_Reactor::instance()->schedule_timer(
        this, nullptr, ACE_Time_Value(1),
        ACE_Time_Value(static_cast<time_t>(m_cfg.interval_sec)));

    ACE_Reactor::instance()->run_reactor_event_loop();
    return 0;
}

int DdnsClient::handle_timeout(const ACE_Time_Value&, const void*) {
    tick();
    return 0;
}

void DdnsClient::tick() {
    if (!m_cfg.enabled) {
        if (m_state != State::Disabled) publish_state(State::Disabled);
        return;
    }
    if (m_cfg.hostname.empty()) { set_error("ddns.hostname is unset"); return; }

    // Rebuild the backend if the provider changed.
    if (!m_backend || m_backend_provider != m_cfg.provider) {
        m_backend = make_backend(m_cfg.provider);
        m_backend_provider = m_cfg.provider;
    }
    if (!m_backend) { set_error("unknown provider: " + m_cfg.provider); return; }

    // Detect the current public IP.
    publish_state(State::Detecting);
    auto ip = m_detector->detect();
    if (!ip) { set_error("public IP detection failed (no egress / clock / echo down)"); return; }
    m_ds.set("ddns.last.ip", data_store::Value{*ip});

    // Idempotent: only call the provider when the IP changed or the forced-
    // refresh window elapsed.
    const long now = now_epoch();
    const bool changed = (*ip != m_last_ip);
    const bool stale   = (m_last_push_ts != 0) &&
                         (now - m_last_push_ts >= static_cast<long>(m_cfg.refresh_force_sec));
    if (!changed && !stale && m_last_push_ts != 0) {
        publish_state(looks_reachable(*ip) ? State::Ok : State::OkUnreachable);
        return;
    }

    publish_state(State::Updating);
    Result res = m_backend->update(m_cfg.hostname, *ip, creds_for_provider());
    m_last_push_ts = now;
    if (!res.ok) {
        set_error(std::string(m_backend_provider) + ": " + res.msg);
        return;
    }
    m_last_ip = *ip;
    m_last_ok_ts = now;
    m_ds.set(std::vector<data_store::KV>{
        {"ddns.last.ip",    data_store::Value{*ip}},
        {"ddns.last.ok.ts", data_store::Value{static_cast<std::int32_t>(now)}},
        {"ddns.last.error", data_store::Value{std::string("")}},
    });
    publish_state(looks_reachable(*ip) ? State::Ok : State::OkUnreachable);
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ddns] updated %C -> %C via %C (%C)\n"),
               m_cfg.hostname.c_str(), ip->c_str(), m_backend_provider.c_str(),
               to_string(m_state)));
}

void DdnsClient::on_config_event(const data_store::Client::Event&) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_cfg_dirty = true;
    ACE_Reactor::instance()->notify(this);
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
    std::string prev_provider = m_cfg.provider;
    load_config_from_ds();

    ACE_DEBUG((LM_INFO,
        ACE_TEXT("%D [ddns] config reload: enabled=%d provider=%C host=%C interval=%us\n"),
        m_cfg.enabled ? 1 : 0, m_cfg.provider.c_str(),
        m_cfg.hostname.empty() ? "(unset)" : m_cfg.hostname.c_str(),
        m_cfg.interval_sec));

    if (m_cfg.provider != prev_provider) m_backend.reset();   // rebuilt next tick
    if (m_cfg.interval_sec != prev_interval) {
        ACE_Reactor::instance()->cancel_timer(this);
        ACE_Reactor::instance()->schedule_timer(
            this, nullptr, ACE_Time_Value(1),
            ACE_Time_Value(static_cast<time_t>(m_cfg.interval_sec)));
    }
    if (!m_cfg.enabled) {
        publish_state(State::Disabled);
    } else if (prev_enabled != m_cfg.enabled) {
        // just turned on → force an immediate push on the next tick
        m_last_push_ts = 0;
        publish_state(State::Detecting);
    }
}

} // namespace ddns
