#include "supervisor.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sstream>
#include <string>
#include <sys/stat.h>

#include <ace/Log_Msg.h>
#include <nlohmann/json.hpp>

#include "data_store/service_gate.hpp"

namespace wifi_client {

// ─────────────────────── Pure helpers ─────────────────────────

namespace {

std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\n') {
            out.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < s.size()) out.emplace_back(s.substr(start));
    return out;
}

std::vector<std::string> split_tabs(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\t') {
            out.emplace_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    out.emplace_back(s.substr(start));
    return out;
}

bool is_integer_literal(const std::string& s) {
    if (s.empty()) return false;
    std::size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
    if (i == s.size()) return false;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

} // namespace

std::vector<ScanEntry> parse_scan_results(const std::string& wpa_reply) {
    std::vector<ScanEntry> rows;
    for (const auto& line : split_lines(wpa_reply)) {
        if (line.empty()) continue;
        auto cols = split_tabs(line);
        if (cols.size() < 5) continue;            // header or junk
        // Tolerate the header line ("bssid\tfrequency\t...") by
        // checking that cols[2] (signal) parses as integer.
        if (!is_integer_literal(cols[2])) continue;
        ScanEntry e;
        e.bssid  = cols[0];
        e.signal = std::atoi(cols[2].c_str());
        e.flags  = cols[3];
        e.ssid   = cols[4];
        rows.push_back(std::move(e));
    }
    return rows;
}

std::string cap_and_serialize_scan_results(std::vector<ScanEntry> rows,
                                           std::size_t            max_count) {
    std::sort(rows.begin(), rows.end(),
              [](const ScanEntry& a, const ScanEntry& b) {
                  return a.signal > b.signal;
              });
    if (rows.size() > max_count) rows.resize(max_count);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : rows) {
        arr.push_back({
            {"ssid",   e.ssid},
            {"bssid",  e.bssid},
            {"signal", e.signal},
            {"flags",  e.flags},
        });
    }
    return arr.dump();
}

bool is_networks_change_additive(const std::string& before_json,
                                 const std::string& after_json) {
    auto parse = [](const std::string& s) -> std::optional<nlohmann::json> {
        try {
            auto doc = nlohmann::json::parse(s.empty() ? std::string("[]") : s);
            if (!doc.is_array()) return std::nullopt;
            return doc;
        } catch (...) { return std::nullopt; }
    };
    auto before = parse(before_json);
    auto after  = parse(after_json);
    if (!before || !after) return false;

    // Every entry in `before` must appear in `after` with identical
    // ssid + psk + key_mgmt + priority. Reorder counts as
    // non-additive (priority is the canonical order signal).
    auto entry_match = [](const nlohmann::json& a,
                          const nlohmann::json& b) {
        auto getstr = [](const nlohmann::json& j, const char* k,
                         const std::string& def = {}) {
            if (j.contains(k) && j[k].is_string()) return j[k].get<std::string>();
            return def;
        };
        auto getint = [](const nlohmann::json& j, const char* k, int def) {
            if (j.contains(k) && j[k].is_number_integer()) return j[k].get<int>();
            return def;
        };
        return getstr(a, "ssid") == getstr(b, "ssid")
            && getstr(a, "psk")  == getstr(b, "psk")
            && getstr(a, "key_mgmt", "WPA-PSK") ==
               getstr(b, "key_mgmt", "WPA-PSK")
            && getint(a, "priority", 0) == getint(b, "priority", 0);
    };

    for (const auto& b : *before) {
        bool found = false;
        for (const auto& a : *after) {
            if (entry_match(b, a)) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

std::optional<std::size_t>
select_best_network(const std::vector<WifiNetwork>& parsed_networks,
                    const std::vector<ScanEntry>&   scan_results) {
    // Find the strongest scan entry whose SSID appears in
    // `parsed_networks`; return that index.
    auto best_signal = std::numeric_limits<int>::min();
    std::optional<std::size_t> best_idx;
    for (std::size_t i = 0; i < parsed_networks.size(); ++i) {
        const auto& cfg = parsed_networks[i];
        for (const auto& s : scan_results) {
            if (s.ssid == cfg.ssid && s.signal > best_signal) {
                best_signal = s.signal;
                best_idx = i;
            }
        }
    }
    return best_idx;
}

bool nm_conflict_detected(const std::string& iface,
                          const std::string& ctrl_dir) {
    // (1) systemctl is-active NetworkManager → "active" means yes.
    // popen() captures stdout; the exit code is conveyed via
    // pclose's wait status. We accept ECONNREFUSED-class failures
    // as "NM not installed" (no conflict).
    FILE* fp = ::popen("systemctl is-active NetworkManager 2>/dev/null", "r");
    if (fp) {
        char buf[32] = {0};
        std::size_t n = ::fread(buf, 1, sizeof(buf) - 1, fp);
        ::pclose(fp);
        if (n >= 6 && std::string(buf, 6) == "active") return true;
    }
    // (2) Existing ctrl socket at <ctrl_dir>/<iface> → conflict.
    std::string path = ctrl_dir;
    if (!path.empty() && path.back() != '/') path.push_back('/');
    path += iface;
    struct ::stat st;
    if (::stat(path.c_str(), &st) == 0) return true;
    return false;
}

bool RssiCoalescer::should_publish(int dbm) {
    auto now = m_clock.now();
    if (!m_last.has_value()) {
        m_last = now;
        m_last_value = dbm;
        return true;          // first publish always emits
    }
    auto elapsed = now - *m_last;
    if (elapsed >= m_window) {
        m_last = now;
        m_last_value = dbm;
        return true;
    }
    // Same value within the window: silently drop. Different value
    // within the window: also drop — operators want the latest
    // sampled value, not every micro-change. The next sample after
    // the window expires gets through.
    (void)m_last_value;
    return false;
}

// ─────────────────────── Supervisor class ────────────────────

Supervisor::Supervisor(DsBridge& ds, SupervisorOptions opt)
  : m_ds(ds),
    m_opt(std::move(opt)),
    m_fsm(Lifecycle::Sinks{
        // set_state -> ds_bridge.set_assoc_state. The Lifecycle's
        // transition() suppresses same-state re-entries, so this
        // write-through satisfies NFR-WIFI-004 by construction.
        [this](std::string_view s) {
            m_ds.set_assoc_state(std::string(s));
        },
        // on_connected: handled in the Supervisor run-loop directly
        // (spawns DHCP, publishes ssid/bssid). Leaving empty here
        // avoids double-publishing.
        nullptr,
        nullptr,  // on_disconnected — same reasoning
        nullptr,  // on_scan_results — Supervisor issues SCAN_RESULTS
        // on_reject -> wifi.last.error so operators see auth/assoc
        // failures without tailing journalctl.
        [this](const std::string& err) {
            m_ds.set_last_error(err);
        },
    }) {
    // L16/D6 — services.wifi.client.enable gate. Constructed lazily
    // here against the DsBridge-owned Client. The watcher thread is
    // intentionally minimal: it just notes that a transition has
    // landed; the run loop reads enabled() at the natural ticks.
    if (auto* cli = m_ds.client()) {
        m_svc = std::make_unique<data_store::ServiceGate>(*cli, "wifi.client");
        m_svc->publish_state("running");
    }
}

Supervisor::~Supervisor() {
    m_svc_stop.store(true, std::memory_order_release);
    if (m_svc) m_svc->shutdown();
    if (m_svc_watcher.joinable()) m_svc_watcher.join();
}

bool Supervisor::initialize() {
    if (nm_conflict_detected(m_opt.iface, m_opt.ctrl_dir)) {
        m_ds.set_assoc_state("conflict");
        m_ds.set_last_error(
            "NetworkManager active or " + m_opt.ctrl_dir + "/" + m_opt.iface
            + " already exists; refusing to spawn wpa_supplicant");
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [wifi:%t] %M %N:%l NM-conflict; refusing start\n")));
        return false;
    }

    std::string err;
    auto networks = parse_wifi_networks(
        m_ds.networks().value_or("[]"), &err);
    if (!err.empty()) {
        m_ds.set_assoc_state("conflict");
        m_ds.set_last_error(err);
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [wifi:%t] %M %N:%l wifi.networks bad: %C\n"),
                   err.c_str()));
        return false;
    }

    if (!m_wpa.spawn_wpa_supplicant(
            m_opt.wpa_path, m_opt.iface, m_opt.ctrl_dir, networks)) {
        m_ds.set_assoc_state("exited");
        m_ds.set_last_error("wpa_supplicant spawn failed");
        return false;
    }
    m_ds.set_pid_wpa(static_cast<std::uint32_t>(m_wpa.pid()));

    // Give wpa_supplicant a moment to bind its control socket.
    // Short busy-wait — 1s total, 50ms polls. Real production would
    // use inotify; for v1 the wpa_supplicant startup time is ~ms.
    std::string sock_path = m_opt.ctrl_dir + "/" + m_opt.iface;
    for (int i = 0; i < 20; ++i) {
        struct ::stat st;
        if (::stat(sock_path.c_str(), &st) == 0) break;
        ::usleep(50 * 1000);
    }
    if (!m_ctrl.connect(sock_path)) {
        m_ds.set_assoc_state("exited");
        m_ds.set_last_error("ctrl connect/ATTACH failed");
        return false;
    }
    m_ds.set_assoc_state("scanning");
    std::string reply;
    m_ctrl.request("SCAN", reply);
    return true;
}

int Supervisor::run() {
    // The integration path is exercised by log/L15/smoke.sh against
    // fake-wpa.sh (D8). The Supervisor's pure helpers + a basic
    // initialize() path are unit-tested in supervisor_test.cpp.

    // L16/D6 — services.wifi.client.enable check. disable dominates
    // the NM-conflict gate: if operator disabled the service, we
    // publish state="disabled" and park immediately, before
    // initialize() probes for NM (avoids spurious "conflict" writes).
    if (m_svc && !m_svc->enabled()) {
        m_ds.set_assoc_state("disconnected");
        m_svc->publish_state("disabled");
        auto v = m_svc->wait();
        if (!v.has_value()) return 0;
    }

    if (!initialize()) return 1;
    if (m_svc) m_svc->publish_state("running");

    // Outer poll loop: recv ctrl events, feed Lifecycle, react.
    while (true) {
        // L16/D6 — operator disable mid-session: reap workers and
        // park within one 200ms recv tick (NFR-SVC-001).
        if (m_svc && !m_svc->enabled()) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D [wifi:%t] %M %N:%l services."
                                "wifi.client.enable=false; reaping "
                                "workers\n")));
            m_svc->publish_state("stopping");
            m_dhcp.terminate();
            m_wpa.terminate();
            m_ctrl.close();
            m_ds.set_pid_wpa(0u);
            m_ds.set_pid_dhcp(0u);
            m_ds.set_assoc_state("disconnected");
            m_svc->publish_state("disabled");
            // Block on the gate; re-init on re-enable.
            auto v = m_svc->wait();
            if (!v.has_value()) return 0;
            if (!initialize()) return 1;
            m_svc->publish_state("running");
            continue;
        }
        auto ev = m_ctrl.recv_event(/*timeout_ms=*/200);
        if (!ev.has_value()) {
            if (!m_wpa.running()) {
                m_ds.set_assoc_state("exited");
                m_ds.set_pid_wpa(0u);
                return 0;
            }
            continue;
        }
        m_fsm.step(*ev);
        if (ev->kind == ctrl::CtrlEvent::Kind::ScanResults) {
            // Issue SCAN_RESULTS, parse, cap+serialize, publish.
            // ctrl::Client::request() defers concurrent CTRL-EVENT
            // datagrams so the CONNECTED event that often follows
            // close on the heels of SCAN_RESULTS isn't swallowed —
            // it lands in the listener's deferred queue and we see
            // it on the next recv_event tick.
            std::string reply;
            if (m_ctrl.request("SCAN_RESULTS", reply)) {
                auto rows = parse_scan_results(reply);
                auto cap  = m_ds.scan_max_results().value_or(20u);
                m_ds.set_scan_results(
                    cap_and_serialize_scan_results(std::move(rows),
                                                   static_cast<std::size_t>(cap)));
                m_ds.set_scan_last_unix(static_cast<std::uint32_t>(
                    std::time(nullptr)));
            }
        } else if (ev->kind == ctrl::CtrlEvent::Kind::Connected) {
            m_ds.set_assoc_ssid(ev->ssid);
            m_ds.set_assoc_bssid(ev->bssid);
            auto dhcp_path = pick_dhcp_client(
                m_ds.dhcp_client().value_or("auto"),
                m_ds.dhcp_path().value_or(""));
            if (!dhcp_path.empty()) {
                if (m_dhcp.spawn_dhcp(dhcp_path, m_opt.iface)) {
                    m_ds.set_dhcp_state("requesting");
                    m_ds.set_pid_dhcp(static_cast<std::uint32_t>(m_dhcp.pid()));
                }
            }
            if (m_opt.once) return 0;
        } else if (ev->kind == ctrl::CtrlEvent::Kind::Disconnected) {
            m_dhcp.terminate();
            m_ds.set_dhcp_state("exited");
            m_ds.set_pid_dhcp(0u);
        } else if (ev->kind == ctrl::CtrlEvent::Kind::Terminating) {
            return 0;
        } else if (ev->kind == ctrl::CtrlEvent::Kind::AssocReject
                || ev->kind == ctrl::CtrlEvent::Kind::AuthReject) {
            m_ds.set_last_error("reject:" + ev->reason);
        }
    }
}

} // namespace wifi_client
