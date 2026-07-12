#include "smsctl/executor.hpp"

#include <nlohmann/json.hpp>

namespace smsctl {

namespace {

using json = nlohmann::json;

/// Read a ds key, rendering an absent/empty value as "-" so a STATUS reply is
/// always well-formed.
std::string field(DsSink& sink, const char* key) {
    auto v = sink.get(key);
    if (!v || v->empty()) return "-";
    return *v;
}

} // namespace

std::string clamp_reply(std::string s) {
    if (s.size() <= Executor::kMaxReply) return s;
    s.resize(Executor::kMaxReply - 3);
    s += "...";
    return s;
}

std::string wifi_networks_upsert(const std::string& networks_json,
                                 const std::string& ssid,
                                 const std::string& psk) {
    json arr = json::array();
    try {
        auto parsed = json::parse(networks_json);
        if (parsed.is_array()) arr = parsed;
        // A malformed/non-array value is replaced rather than propagated: the
        // operator is texting us BECAUSE WiFi is broken, so refusing to write
        // over junk would strand them.
    } catch (const std::exception&) {
        // same — start a fresh array
    }

    json entry;
    entry["ssid"] = ssid;
    if (psk.empty()) {
        entry["key_mgmt"] = "NONE";          // open network — no psk field
    } else {
        entry["psk"]      = psk;
        entry["key_mgmt"] = "WPA-PSK";
    }

    bool replaced = false;
    for (auto& e : arr) {
        if (e.is_object() && e.value("ssid", std::string()) == ssid) {
            // Preserve an operator-set priority; everything else is replaced.
            if (e.contains("priority")) entry["priority"] = e["priority"];
            e = entry;
            replaced = true;
            break;
        }
    }
    if (!replaced) arr.push_back(entry);
    return arr.dump();
}

std::string Executor::handle(const Command& cmd, const std::string& sender,
                             std::uint64_t now, std::uint64_t seed) {
    const std::string kw = cmd.keyword();

    if (cmd.kind == Kind::Unknown)
        return clamp_reply("ERR: " + cmd.error);

    // ── LOGIN — the only command that needs no session ──────────────────
    if (cmd.kind == Kind::Login) {
        const std::string err = m_sessions.login(sender, cmd.args[0],
                                                 cmd.args[1], m_lookup, now);
        if (!err.empty()) return clamp_reply("ERR LOGIN: " + err);
        const auto* s = m_sessions.session(sender, now);
        const std::uint64_t mins = m_sessions.config().session_ttl_sec / 60;
        return clamp_reply("OK LOGIN: " + (s ? s->id : std::string("?")) +
                           ", " + std::to_string(mins) + " min");
    }

    if (cmd.kind == Kind::Logout) {
        m_sessions.logout(sender);
        return "OK LOGOUT";
    }

    // ── everything else needs a live session ────────────────────────────
    const Account* acct = m_sessions.session(sender, now);
    if (!acct)
        return clamp_reply("ERR " + kw + ": login required (IOT LOGIN <user> <password>)");

    // Mutating commands need Admin; a Viewer may still read STATUS.
    if (is_mutating(cmd.kind) && acct->access != "Admin")
        return clamp_reply("ERR " + kw + ": admin access required");

    switch (cmd.kind) {
        case Kind::Status:       return do_status();
        case Kind::Reboot:       return do_reboot();
        case Kind::FactoryReset: return do_factory_reset(cmd, sender, now, seed);
        case Kind::Apn:          return do_apn(cmd.args[0]);
        case Kind::RadioRestart: return do_radio_restart();
        case Kind::Wifi:         return do_wifi(cmd.args[0],
                                                cmd.args.size() > 1 ? cmd.args[1]
                                                                    : std::string());
        default:                 return clamp_reply("ERR " + kw + ": not supported");
    }
}

std::string Executor::do_status() {
    // One GSM-7 SMS. cs= is called out separately because MT-SMS rides the CS
    // domain on 2G: a device can be data-"home" while CS is still searching,
    // which silently kills SMS delivery (HW-confirmed 2026-07-11).
    std::string s = "OK STATUS: reg=" + field(m_sink, "cell.reg") +
                    " cs="   + field(m_sink, "cell.reg.cs") +
                    " sig="  + field(m_sink, "cell.signal.dbm") + "dBm" +
                    " ip="   + field(m_sink, "cell.ip") +
                    " vpn="  + field(m_sink, "vpn.state") +
                    " wifi=" + field(m_sink, "wifi.assoc.ssid") +
                    " if="   + field(m_sink, "net.iface.active");
    return clamp_reply(std::move(s));
}

std::string Executor::do_reboot() {
    if (!m_sink.arm_trigger(kRebootTrigger, "reboot\n"))
        return "ERR REBOOT: cannot arm trigger";
    return "OK REBOOT: rebooting now";
}

std::string Executor::do_factory_reset(const Command& cmd,
                                       const std::string& sender,
                                       std::uint64_t now, std::uint64_t seed) {
    // Two-step: the most destructive command must not be one typo away.
    if (cmd.args.empty()) {
        const std::string nonce = m_sessions.mint_nonce(sender, now, seed);
        return clamp_reply("OK FACTORY-RESET: confirm within 5 min — "
                           "reply: IOT FACTORY-RESET " + nonce);
    }
    if (!m_sessions.consume_nonce(sender, cmd.args[0], now))
        return "ERR FACTORY-RESET: bad or expired code";
    if (!m_sink.arm_trigger(kFactoryResetTrigger, "factory-reset\n"))
        return "ERR FACTORY-RESET: cannot arm trigger";
    return "OK FACTORY-RESET: wiping + rebooting";
}

std::string Executor::do_apn(const std::string& apn) {
    if (!m_sink.set("cell.apn", apn))
        return "ERR APN: ds write failed";
    // The radio cycle re-reads config and re-issues AT+CGDCONT with the new
    // APN (cellular-client start_reset → load_config_from_ds).
    if (!m_sink.set("cell.reset.request", std::to_string(m_sink.now_ms())))
        return "ERR APN: saved, but radio restart failed";
    return clamp_reply("OK APN: " + apn + " saved, restarting radio");
}

std::string Executor::do_radio_restart() {
    if (!m_sink.set("cell.reset.request", std::to_string(m_sink.now_ms())))
        return "ERR RADIO: ds write failed";
    return "OK RADIO: restarting (CFUN cycle)";
}

std::string Executor::do_wifi(const std::string& ssid, const std::string& psk) {
    const std::string cur = m_sink.get("wifi.networks").value_or("[]");
    const std::string next = wifi_networks_upsert(cur, ssid, psk);
    // The write IS the command: iot-wifi-client watches wifi.networks and
    // re-applies (respawns wpa_supplicant with the new credentials) within one
    // 200ms tick. smsctld owns no WiFi machinery of its own.
    if (!m_sink.set("wifi.networks", next))
        return "ERR WIFI: ds write failed";
    return clamp_reply("OK WIFI: \"" + ssid + "\" saved, reconnecting");
}

} // namespace smsctl
