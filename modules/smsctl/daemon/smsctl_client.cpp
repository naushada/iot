#include "smsctl_client.hpp"

#include <fstream>
#include <vector>

#include <ace/Log_Msg.h>
#include <ace/OS_NS_sys_time.h>
#include <ace/Reactor.h>
#include <ace/Time_Value.h>

#include <nlohmann/json.hpp>

#include "smsctl/parser.hpp"

namespace smsctl {

namespace {

using json = nlohmann::json;

/// Keys the daemon reacts to. sms.version is cellular-client's bump-on-change
/// counter for the whole sms.* domain — watching it (rather than sms.last.text)
/// also catches a repeat of the identical text from the same sender.
const char* const kWatchKeys[] = {
    "sms.version",
    "smsctl.enabled",
    "smsctl.allowed.numbers",
    "smsctl.session.ttl.sec",
    "smsctl.lockout.failures",
    "smsctl.lockout.sec",
};

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else if (c != ' ' && c != '\t')  { cur.push_back(c); }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

/// An alphanumeric sender ("AZ-AIRTEL-S") cannot receive SMS — replying would
/// silently burn credit forever. Only reply to something that looks E.164.
bool is_reachable_sender(const std::string& s) {
    std::size_t digits = 0;
    for (char c : s) {
        if (c >= '0' && c <= '9') ++digits;
        else if (c != '+' && c != ' ' && c != '-') return false;
    }
    return digits >= 7;
}

/// The live ds/filesystem implementation of the executor's world.
class LiveSink : public DsSink {
public:
    explicit LiveSink(data_store::Client& ds) : m_ds(ds) {}

    bool set(const std::string& key, const std::string& value) override {
        auto rs = m_ds.set(key, data_store::Value{value});
        if (!rs.ok)
            ACE_ERROR((LM_ERROR, ACE_TEXT("%D [smsctl] ds set(%C) failed: %C\n"),
                       key.c_str(), rs.err.c_str()));
        return rs.ok;
    }

    std::optional<std::string> get(const std::string& key) override {
        std::vector<data_store::Client::GetResult> got;
        if (!m_ds.get({key}, got).ok || got.empty() || !got[0].has_value)
            return std::nullopt;
        return data_store::to_string(got[0].value);
    }

    bool arm_trigger(const std::string& path,
                     const std::string& content) override {
        // /run/iot is 2775 root:iot — this daemon runs in group iot, so it can
        // create the trigger; the root .path unit does the privileged work.
        std::ofstream trig(path, std::ios::trunc);
        if (!trig) {
            ACE_ERROR((LM_ERROR,
                       ACE_TEXT("%D [smsctl] cannot arm trigger %C "
                                "(perms or non-Yocto host)\n"), path.c_str()));
            return false;
        }
        trig << content;
        ACE_DEBUG((LM_WARNING,
                   ACE_TEXT("%D [smsctl] armed privileged trigger %C\n"),
                   path.c_str()));
        return trig.good();
    }

    std::uint64_t now_ms() override {
        const ACE_Time_Value tv = ACE_OS::gettimeofday();
        return static_cast<std::uint64_t>(tv.sec()) * 1000ULL +
               static_cast<std::uint64_t>(tv.usec() / 1000);
    }

private:
    data_store::Client& m_ds;
};

} // namespace

void SmsCtlClient::load_config_from_ds() {
    auto get_str = [this](const char* key) -> std::string {
        std::vector<data_store::Client::GetResult> got;
        if (m_ds.get({std::string(key)}, got).ok && !got.empty() && got[0].has_value)
            if (auto s = data_store::to_string(got[0].value)) return *s;
        return {};
    };
    auto get_u32 = [this](const char* key, std::uint32_t dflt) -> std::uint32_t {
        std::vector<data_store::Client::GetResult> got;
        if (m_ds.get({std::string(key)}, got).ok && !got.empty() && got[0].has_value)
            if (auto n = data_store::to_int32(got[0].value))
                if (*n > 0) return static_cast<std::uint32_t>(*n);
        return dflt;
    };
    auto get_bool = [this](const char* key, bool dflt) -> bool {
        std::vector<data_store::Client::GetResult> got;
        if (m_ds.get({std::string(key)}, got).ok && !got.empty() && got[0].has_value)
            if (auto b = data_store::to_bool(got[0].value)) return *b;
        return dflt;
    };

    m_enabled = get_bool("smsctl.enabled", false);

    smsctl::Config c;
    c.session_ttl_sec  = get_u32("smsctl.session.ttl.sec", 600);
    c.lockout_failures = get_u32("smsctl.lockout.failures", 5);
    c.lockout_sec      = get_u32("smsctl.lockout.sec", 900);
    c.allowed_numbers  = split_csv(get_str("smsctl.allowed.numbers"));
    m_sessions.set_config(std::move(c));
}

bool SmsCtlClient::lookup_account(const std::string& id, Account& out) {
    // Mirrors iot-httpd's login resolution: the built-in "admin" lives in its
    // own keys; every other user is an entry in the auth.users.accounts JSON
    // array. Same hashes → SMS login accepts exactly the device-ui passwords.
    std::vector<data_store::Client::GetResult> got;
    if (id == "admin") {
        if (!m_ds.get({std::string("auth.users.admin.password.hash")}, got).ok ||
            got.empty() || !got[0].has_value)
            return false;
        auto h = data_store::to_string(got[0].value);
        if (!h || h->empty()) return false;
        out.id     = "admin";
        out.hash   = *h;
        out.access = "Admin";
        got.clear();
        if (m_ds.get({std::string("auth.users.admin.access")}, got).ok &&
            !got.empty() && got[0].has_value)
            if (auto a = data_store::to_string(got[0].value))
                if (*a == "Viewer") out.access = "Viewer";
        return true;
    }

    if (!m_ds.get({std::string("auth.users.accounts")}, got).ok ||
        got.empty() || !got[0].has_value)
        return false;
    auto s = data_store::to_string(got[0].value);
    if (!s || s->empty()) return false;
    try {
        const auto arr = json::parse(*s);
        if (!arr.is_array()) return false;
        for (const auto& u : arr) {
            if (!u.is_object()) continue;
            if (u.value("id", std::string()) != id) continue;
            out.id     = id;
            out.hash   = u.value("hash", std::string());
            out.access = u.value("access", std::string("Viewer"));
            return !out.hash.empty();
        }
    } catch (const std::exception&) {
        return false;
    }
    return false;
}

void SmsCtlClient::publish_state() {
    std::vector<data_store::KV> batch;
    batch.emplace_back("smsctl.state",
                       data_store::Value{std::string(m_enabled ? "listening"
                                                               : "disabled")});
    batch.emplace_back("smsctl.sessions",
                       data_store::Value{std::to_string(m_sessions.session_count())});
    if (!m_ds.set_volatile(batch).ok)
        ACE_ERROR((LM_ERROR, ACE_TEXT("%D [smsctl] ds set(smsctl.state) failed\n")));
}

void SmsCtlClient::drain_inbound() {
    std::vector<data_store::Client::GetResult> got;
    if (!m_ds.get({std::string("sms.last.sender"), std::string("sms.last.text"),
                   std::string("sms.last.ts")}, got).ok || got.size() < 3)
        return;

    auto val = [&](std::size_t i) -> std::string {
        if (!got[i].has_value) return {};
        return data_store::to_string(got[i].value).value_or(std::string());
    };
    const std::string sender = val(0);
    const std::string text   = val(1);
    const std::string ts     = val(2);
    if (sender.empty() || text.empty()) return;

    {   // Dedupe: the same (sender, text, ts) is processed exactly once. This
        // is also the replay guard — the tuple is baselined at startup, so SMS
        // already in the SIM store cannot execute after a daemon restart.
        std::lock_guard<std::mutex> lk(m_mtx);
        if (sender == m_seen_sender && text == m_seen_text && ts == m_seen_ts)
            return;
        m_seen_sender = sender;
        m_seen_text   = text;
        m_seen_ts     = ts;
    }

    if (!m_enabled) return;                      // parked

    // Non-allowlisted senders are dropped in SILENCE: no reply, so the device
    // is not an oracle for "is smsctl on?" and carrier spam costs us nothing.
    if (!m_sessions.sender_allowed(sender)) {
        ACE_DEBUG((LM_INFO,
                   ACE_TEXT("%D [smsctl] ignoring SMS from non-allowed sender %C\n"),
                   sender.c_str()));
        return;
    }

    const Command cmd = parse(text);
    if (cmd.kind == Kind::NotACommand) return;   // ordinary text — stay silent

    const ACE_Time_Value tv = ACE_OS::gettimeofday();
    const std::uint64_t now  = static_cast<std::uint64_t>(tv.sec());
    // Nonce entropy: the microsecond clock mixed with the message count. Good
    // enough for a 5-minute, single-use, session-gated confirmation code.
    const std::uint64_t seed = static_cast<std::uint64_t>(tv.usec()) * 2654435761ULL
                             + ++m_handled;

    LiveSink sink(m_ds);
    Executor ex(sink, m_sessions,
                [this](const std::string& id, Account& out) {
                    return lookup_account(id, out);
                });
    const std::string reply = ex.handle(cmd, sender, now, seed);

    // NEVER log the command arguments — a LOGIN password or a WiFi PSK must not
    // reach the journal. Keyword + outcome only.
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [smsctl] %C from %C -> %C\n"),
               cmd.keyword(), sender.c_str(),
               reply.compare(0, 2, "OK") == 0 ? "ok" : "err"));

    // Same rule for ds: keyword only, never arguments.
    std::vector<data_store::KV> batch;
    batch.emplace_back("smsctl.last.sender", data_store::Value{sender});
    batch.emplace_back("smsctl.last.cmd",    data_store::Value{std::string(cmd.keyword())});
    batch.emplace_back("smsctl.last.result",
                       data_store::Value{reply.compare(0, 2, "OK") == 0
                                             ? std::string("ok")
                                             : std::string("err")});
    batch.emplace_back("smsctl.last.ts",  data_store::Value{std::to_string(now)});
    batch.emplace_back("smsctl.sessions",
                       data_store::Value{std::to_string(m_sessions.session_count())});
    batch.emplace_back("smsctl.version",  data_store::Value{std::to_string(m_handled)});
    m_ds.set_volatile(batch);

    if (!is_reachable_sender(sender)) {
        ACE_DEBUG((LM_INFO,
                   ACE_TEXT("%D [smsctl] sender %C is not E.164 — no reply sent\n"),
                   sender.c_str()));
        return;
    }

    // Reply over the proven MO envelope: set the payload, then bump the request
    // token that cellular-client watches.
    std::vector<data_store::KV> out;
    out.emplace_back("sms.send.to",   data_store::Value{sender});
    out.emplace_back("sms.send.text", data_store::Value{reply});
    if (!m_ds.set(out).ok ||
        !m_ds.set("sms.send.request",
                  data_store::Value{std::to_string(sink.now_ms())}).ok) {
        ACE_ERROR((LM_ERROR, ACE_TEXT("%D [smsctl] reply send failed\n")));
    }
}

int SmsCtlClient::handle_timeout(const ACE_Time_Value&, const void*) {
    const std::uint64_t now =
        static_cast<std::uint64_t>(ACE_OS::gettimeofday().sec());
    const std::size_t before = m_sessions.session_count();
    m_sessions.sweep(now);
    if (m_sessions.session_count() != before) publish_state();
    return 0;
}

int SmsCtlClient::handle_exception(ACE_HANDLE) {
    bool cfg_dirty, sms_dirty;
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        cfg_dirty = m_cfg_dirty; m_cfg_dirty = false;
        sms_dirty = m_sms_dirty; m_sms_dirty = false;
    }
    if (cfg_dirty) {
        const bool was = m_enabled;
        load_config_from_ds();
        if (was != m_enabled)
            ACE_DEBUG((LM_INFO, ACE_TEXT("%D [smsctl] %C\n"),
                       m_enabled ? "enabled — listening for IOT commands"
                                 : "disabled — inbound SMS ignored"));
        publish_state();
    }
    if (sms_dirty) drain_inbound();
    return 0;
}

int SmsCtlClient::run() {
    if (!m_ds.connect(m_cfg.ds_sock).ok)
        ACE_ERROR_RETURN((LM_ERROR, ACE_TEXT("%D [smsctl] ds connect failed\n")), 1);

    load_config_from_ds();

    // Replay guard: whatever SMS is currently in sms.last.* predates us (it may
    // have been drained from the SIM store at boot). Record it as already seen
    // so a restart never re-executes an old command.
    {
        std::vector<data_store::Client::GetResult> got;
        if (m_ds.get({std::string("sms.last.sender"), std::string("sms.last.text"),
                      std::string("sms.last.ts")}, got).ok && got.size() >= 3) {
            auto val = [&](std::size_t i) -> std::string {
                if (!got[i].has_value) return {};
                return data_store::to_string(got[i].value).value_or(std::string());
            };
            std::lock_guard<std::mutex> lk(m_mtx);
            m_seen_sender = val(0);
            m_seen_text   = val(1);
            m_seen_ts     = val(2);
        }
    }

    std::vector<std::string> keys;
    for (const char* k : kWatchKeys) keys.emplace_back(k);
    data_store::Client::WatchHandle wh = data_store::Client::kInvalidHandle;
    m_ds.watch(keys, [this](const data_store::Client::Event& ev) {
        // ds LISTENER thread — only flag + wake the reactor. All ds/exec work
        // happens on the reactor thread in handle_exception().
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (ev.key == "sms.version") m_sms_dirty = true;
            else                         m_cfg_dirty = true;
        }
        ACE_Reactor::instance()->notify(this);
    }, &wh);

    publish_state();

    // 1s sweep for session/nonce expiry. No modem or network traffic.
    ACE_Reactor::instance()->schedule_timer(
        this, nullptr, ACE_Time_Value(1), ACE_Time_Value(1));

    ACE_DEBUG((LM_INFO,
        ACE_TEXT("%D [smsctl] up: %C, session ttl %us, lockout %u fail/%us, "
                 "allowlist %u number(s)\n"),
        m_enabled ? "enabled" : "disabled (smsctl.enabled=false)",
        m_sessions.config().session_ttl_sec,
        m_sessions.config().lockout_failures,
        m_sessions.config().lockout_sec,
        static_cast<unsigned>(m_sessions.config().allowed_numbers.size())));

    ACE_Reactor::instance()->run_reactor_event_loop();
    return 0;
}

} // namespace smsctl
