#include "process.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <utility>

#include <ace/Log_Msg.h>
#include <ace/Process.h>

#include <nlohmann/json.hpp>

namespace wifi_client {

namespace {

// wpa_supplicant.conf is line-oriented and has no escape for control
// characters; a stray '\n' in any emitted value would break the conf
// or inject a directive (the conf writer's esc() only handles " and \).
// Reject control characters at parse time instead.
bool has_control_char(const std::string& s) {
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7f) return true;
    }
    return false;
}

} // namespace

// ─────────────────────── Pure helpers ───────────────────────────────

std::vector<WifiNetwork>
parse_wifi_networks(const std::string& json, std::string* err_out) {
    std::vector<WifiNetwork> out;
    auto set_err = [err_out](const std::string& m) {
        if (err_out) *err_out = m;
    };
    if (json.empty()) return out;  // accept "" as same shape as "[]"

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(json);
    } catch (const std::exception& e) {
        set_err(std::string("bad_networks_json: ") + e.what());
        return {};
    }
    if (!doc.is_array()) {
        set_err("bad_networks_json: root must be an array");
        return {};
    }
    out.reserve(doc.size());
    for (std::size_t i = 0; i < doc.size(); ++i) {
        const auto& e = doc[i];
        if (!e.is_object()) {
            set_err("bad_networks_json: entry " + std::to_string(i)
                    + " is not an object");
            return {};
        }
        WifiNetwork n;
        // ssid (required)
        if (!e.contains("ssid") || !e["ssid"].is_string() ||
            e["ssid"].get<std::string>().empty()) {
            set_err("bad_networks_json: entry " + std::to_string(i)
                    + " missing non-empty 'ssid'");
            return {};
        }
        n.ssid = e["ssid"].get<std::string>();
        // key_mgmt (optional, defaults WPA-PSK)
        if (e.contains("key_mgmt")) {
            if (!e["key_mgmt"].is_string()) {
                set_err("bad_networks_json: entry " + std::to_string(i)
                        + " 'key_mgmt' must be string");
                return {};
            }
            n.key_mgmt = e["key_mgmt"].get<std::string>();
        }
        if (n.key_mgmt == "WPA-EAP") {
            // WPA-Enterprise: identity + password instead of psk.
            if (!e.contains("identity") || !e["identity"].is_string() ||
                e["identity"].get<std::string>().empty()) {
                set_err("bad_networks_json: entry " + std::to_string(i)
                        + " missing non-empty 'identity' for key_mgmt=WPA-EAP");
                return {};
            }
            if (!e.contains("password") || !e["password"].is_string() ||
                e["password"].get<std::string>().empty()) {
                set_err("bad_networks_json: entry " + std::to_string(i)
                        + " missing non-empty 'password' for key_mgmt=WPA-EAP");
                return {};
            }
            n.identity = e["identity"].get<std::string>();
            n.password = e["password"].get<std::string>();
            // eap defaults to PEAP, phase2 to MSCHAPV2; ca_cert optional.
            n.eap    = (e.contains("eap") && e["eap"].is_string() &&
                        !e["eap"].get<std::string>().empty())
                           ? e["eap"].get<std::string>()
                           : "PEAP";
            n.phase2 = (e.contains("phase2") && e["phase2"].is_string() &&
                        !e["phase2"].get<std::string>().empty())
                           ? e["phase2"].get<std::string>()
                           : "auth=MSCHAPV2";
            if (e.contains("ca_cert") && e["ca_cert"].is_string()) {
                n.ca_cert = e["ca_cert"].get<std::string>();
            }
        } else if (n.key_mgmt != "NONE") {
            // psk (required unless key_mgmt == NONE)
            if (!e.contains("psk") || !e["psk"].is_string()) {
                set_err("bad_networks_json: entry " + std::to_string(i)
                        + " missing 'psk' for key_mgmt=" + n.key_mgmt);
                return {};
            }
            n.psk = e["psk"].get<std::string>();
        } else if (e.contains("psk") && e["psk"].is_string()) {
            // For NONE we ignore any psk silently — operator may
            // have copy-pasted it; not a hard error.
            n.psk.clear();
        }
        // priority (optional, default 0)
        if (e.contains("priority")) {
            if (!e["priority"].is_number_integer()) {
                set_err("bad_networks_json: entry " + std::to_string(i)
                        + " 'priority' must be integer");
                return {};
            }
            n.priority = e["priority"].get<int>();
        }
        // Every value below is emitted verbatim into wpa_supplicant.conf;
        // reject control characters so a newline can't break the conf or
        // inject a directive.
        for (const auto* f : {&n.ssid, &n.psk, &n.identity, &n.password,
                              &n.eap, &n.phase2, &n.ca_cert}) {
            if (has_control_char(*f)) {
                set_err("bad_networks_json: entry " + std::to_string(i)
                        + " field contains control characters");
                return {};
            }
        }
        out.push_back(std::move(n));
    }
    return out;
}

std::string build_wpa_supplicant_config(const std::string&             iface,
                                        const std::string&             ctrl_dir,
                                        const std::vector<WifiNetwork>& networks) {
    (void)iface;  // wpa_supplicant takes -i on the CLI; the conf
                  // file doesn't repeat it.
    std::ostringstream ss;
    ss << "# Auto-generated by wifi-client L15/D5 — do not edit.\n";
    ss << "ctrl_interface=DIR=" << ctrl_dir << "\n";
    ss << "ctrl_interface_group=0\n";
    ss << "update_config=0\n";
    ss << "\n";

    // Sort by descending priority before emit. wpa_supplicant
    // *also* respects the in-block "priority" field, but emit-order
    // helps a human reader see which SSID the daemon preferred.
    std::vector<WifiNetwork> sorted(networks);
    std::sort(sorted.begin(), sorted.end(),
              [](const WifiNetwork& a, const WifiNetwork& b) {
                  return a.priority > b.priority;
              });

    // Quote-escape any literal " or \\ inside a value so a mischievous
    // passphrase / identity / password can't break the conf parser.
    auto esc = [](const std::string& in) {
        std::string out;
        out.reserve(in.size() + 2);
        for (char c : in) {
            if (c == '"' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
        return out;
    };

    for (const auto& n : sorted) {
        ss << "network={\n";
        // ssid is always quoted; psk may be quoted (passphrase)
        // or a 64-char hex pre-shared key — we always quote here
        // because operators typing PSKs into ds-cli will use the
        // passphrase form, not pre-computed hex.
        ss << "    ssid=\"" << n.ssid << "\"\n";
        if (n.key_mgmt == "NONE") {
            ss << "    key_mgmt=NONE\n";
        } else if (n.key_mgmt == "WPA-EAP") {
            // WPA-Enterprise: no psk — emit EAP method + credentials.
            ss << "    key_mgmt=WPA-EAP\n";
            ss << "    eap=" << (n.eap.empty() ? "PEAP" : n.eap) << "\n";
            ss << "    identity=\"" << esc(n.identity) << "\"\n";
            ss << "    password=\"" << esc(n.password) << "\"\n";
            if (!n.phase2.empty()) {
                ss << "    phase2=\"" << esc(n.phase2) << "\"\n";
            }
            if (!n.ca_cert.empty()) {
                ss << "    ca_cert=\"" << esc(n.ca_cert) << "\"\n";
            }
        } else {
            ss << "    psk=\"" << esc(n.psk) << "\"\n";
            if (n.key_mgmt != "WPA-PSK") {
                ss << "    key_mgmt=" << n.key_mgmt << "\n";
            }
        }
        if (n.priority != 0) {
            ss << "    priority=" << n.priority << "\n";
        }
        ss << "}\n\n";
    }
    return ss.str();
}

std::string write_temp_config(const std::string& body) {
    auto try_open = [](std::string tmpl) -> std::pair<int, std::string> {
        std::vector<char> buf(tmpl.begin(), tmpl.end());
        buf.push_back('\0');
        int fd = ::mkstemps(buf.data(), 5);   // 5 == strlen(".conf")
        if (fd >= 0) return { fd, std::string(buf.data()) };
        return { -1, std::string{} };
    };

    std::string base;
    if (const char* env = ::getenv("TMPDIR"); env && env[0]) base = env;
    else                                                    base = "/tmp";

    auto [fd, path] = try_open(base + "/wpa-XXXXXX.conf");
    if (fd < 0 && base == "/tmp") {
        ::mkdir("/tmp", 01777);
        std::tie(fd, path) = try_open("/tmp/wpa-XXXXXX.conf");
    }
    if (fd < 0) {
        std::tie(fd, path) = try_open("./wpa-XXXXXX.conf");
    }
    if (fd < 0) {
        throw std::runtime_error(
            std::string("mkstemps failed: ") + std::strerror(errno));
    }
    ssize_t wrote = 0;
    while (wrote < static_cast<ssize_t>(body.size())) {
        ssize_t n = ::write(fd, body.data() + wrote,
                            body.size() - static_cast<std::size_t>(wrote));
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            ::close(fd);
            ::unlink(path.c_str());
            throw std::runtime_error(
                std::string("write failed: ") + std::strerror(e));
        }
        wrote += n;
    }
    if (::close(fd) < 0) {
        int e = errno;
        ::unlink(path.c_str());
        throw std::runtime_error(
            std::string("close failed: ") + std::strerror(e));
    }
    return path;
}

std::string pick_dhcp_client(const std::string& scheme,
                             const std::string& override_path) {
    if (!override_path.empty()) return override_path;

    auto exists = [](const std::string& p) {
        struct ::stat st;
        return ::stat(p.c_str(), &st) == 0 && (st.st_mode & S_IXUSR);
    };

    static const char* kUdhcpc[]  = { "/usr/bin/udhcpc",
                                      "/usr/sbin/udhcpc",
                                      "/sbin/udhcpc",
                                      nullptr };
    static const char* kDhclient[] = { "/usr/sbin/dhclient",
                                       "/sbin/dhclient",
                                       nullptr };

    auto first_extant = [&](const char* const* tbl) -> std::string {
        for (auto p = tbl; *p; ++p) if (exists(*p)) return *p;
        return {};
    };

    if (scheme == "udhcpc")   return first_extant(kUdhcpc);
    if (scheme == "dhclient") return first_extant(kDhclient);
    // "auto" (or anything else)
    auto p = first_extant(kUdhcpc);
    if (!p.empty()) return p;
    return first_extant(kDhclient);
}

// ─────────────────────── Process ───────────────────────────────────

Process::Process() = default;

Process::~Process() {
    if (m_pid != 0 && !m_waited) {
        // Default 500ms grace at destruction — fast cleanup;
        // the supervisor's deliberate terminate() uses 5s.
        terminate(std::chrono::milliseconds(500));
    }
    if (!m_config_path.empty()) {
        ::unlink(m_config_path.c_str());
    }
}

bool Process::spawn(const std::string&              executable,
                    const std::vector<std::string>& argv) {
    ACE_Process_Options opts;
    auto quote_if_needed = [](const std::string& s) -> std::string {
        if (s.find_first_of(" \t\n\"\\") == std::string::npos) return s;
        std::string out;
        out.push_back('"');
        for (char c : s) {
            if (c == '"' || c == '\\') out.push_back('\\');
            out.push_back(c);
        }
        out.push_back('"');
        return out;
    };
    std::ostringstream cmd;
    cmd << executable;
    for (std::size_t i = 1; i < argv.size(); ++i) {
        cmd << " " << quote_if_needed(argv[i]);
    }
    opts.command_line("%s", cmd.str().c_str());

    ACE_Process proc;
    pid_t p = proc.spawn(opts);
    if (p == ACE_INVALID_PID) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [wifi:%t] %M %N:%l spawn(%C) failed errno=%d\n"),
                   executable.c_str(), errno));
        return false;
    }
    m_pid = p;
    m_exit_code = -1;
    m_waited = false;
    return true;
}

bool Process::spawn_wpa_supplicant(const std::string&             wpa_path,
                                   const std::string&             iface,
                                   const std::string&             ctrl_dir,
                                   const std::vector<WifiNetwork>& networks,
                                   const std::string&             driver_list) {
    std::string body = build_wpa_supplicant_config(iface, ctrl_dir, networks);
    try {
        m_config_path = write_temp_config(body);
    } catch (const std::exception& e) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [wifi:%t] %M %N:%l write_temp_config "
                            "failed: %C\n"),
                   e.what()));
        return false;
    }
    std::vector<std::string> argv;
    argv.push_back(wpa_path);
    argv.push_back("-i");
    argv.push_back(iface);
    argv.push_back("-c");
    argv.push_back(m_config_path);
    argv.push_back("-C");
    argv.push_back(ctrl_dir);
    if (!driver_list.empty()) {
        argv.push_back("-D");
        argv.push_back(driver_list);
    }
    return spawn(wpa_path, argv);
}

bool Process::spawn_dhcp(const std::string& dhcp_path,
                         const std::string& iface) {
    std::vector<std::string> argv;
    argv.push_back(dhcp_path);
    // Detect udhcpc vs dhclient by basename. The picker hands us
    // an absolute path; we don't need a separate scheme field.
    const auto slash = dhcp_path.find_last_of('/');
    const auto basename = (slash == std::string::npos)
        ? dhcp_path : dhcp_path.substr(slash + 1);
    if (basename.find("udhcpc") != std::string::npos) {
        // udhcpc -i <iface> -f (no daemonise) -q (quit on lease)
        argv.push_back("-i");
        argv.push_back(iface);
        argv.push_back("-f");
        argv.push_back("-q");
    } else {
        // dhclient -d (no daemonise) <iface>
        argv.push_back("-d");
        argv.push_back(iface);
    }
    return spawn(dhcp_path, argv);
}

bool Process::running() {
    if (m_pid == 0 || m_waited) return false;
    int status = 0;
    pid_t r = ::waitpid(m_pid, &status, WNOHANG);
    if (r == 0) return true;
    if (r < 0)  return false;
    m_waited = true;
    if (WIFEXITED(status))         m_exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))  m_exit_code = -1;
    return false;
}

int Process::wait() {
    if (m_waited) return m_exit_code;
    if (m_pid == 0) return -1;
    int status = 0;
    pid_t r;
    do {
        r = ::waitpid(m_pid, &status, 0);
    } while (r < 0 && errno == EINTR);
    m_waited = true;
    if (r < 0) {
        m_exit_code = -1;
        return -1;
    }
    if (WIFEXITED(status))        m_exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) m_exit_code = -1;
    return m_exit_code;
}

void Process::terminate(std::chrono::milliseconds grace) {
    if (m_pid == 0 || m_waited) return;
    ::kill(m_pid, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + grace;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!running()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!m_waited) {
        ::kill(m_pid, SIGKILL);
        wait();
    }
}

} // namespace wifi_client
