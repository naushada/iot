#include "oci_bundle.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

#include <nlohmann/json.hpp>

namespace containers {

namespace {

using json = nlohmann::json;

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool all_digits(const std::string& x) {
    return !x.empty() &&
           std::all_of(x.begin(), x.end(),
                       [](unsigned char c) { return std::isdigit(c) != 0; });
}

} // namespace

ImageConfig parse_image_config(const std::string& body) {
    ImageConfig c;
    try {
        auto j = json::parse(body);
        const json& cfg = (j.contains("config") && j["config"].is_object()) ? j["config"] : j;
        auto arr = [&](const char* k, std::vector<std::string>& out) {
            if (cfg.contains(k) && cfg[k].is_array())
                for (const auto& e : cfg[k])
                    if (e.is_string()) out.push_back(e.get<std::string>());
        };
        arr("Env", c.env);
        arr("Entrypoint", c.entrypoint);
        arr("Cmd", c.cmd);
        if (cfg.contains("WorkingDir") && cfg["WorkingDir"].is_string())
            c.working_dir = cfg["WorkingDir"].get<std::string>();
        if (cfg.contains("User") && cfg["User"].is_string())
            c.user = cfg["User"].get<std::string>();
        c.ok = true;
    } catch (...) {
        c = ImageConfig{};
    }
    return c;
}

std::vector<std::string> parse_args_field(const std::string& field) {
    std::vector<std::string> out;
    const std::string s = trim(field);
    if (s.empty()) return out;
    if (s.front() == '[') {
        try {
            auto j = json::parse(s);
            if (j.is_array()) {
                for (const auto& e : j)
                    if (e.is_string()) out.push_back(e.get<std::string>());
                return out;
            }
        } catch (...) {
            out.clear();   // fall through to whitespace split
        }
    }
    std::istringstream iss(s);
    std::string tok;
    while (iss >> tok) out.push_back(tok);
    return out;
}

std::vector<std::string> resolve_process_args(
    const std::vector<std::string>& img_entrypoint,
    const std::vector<std::string>& img_cmd,
    const std::vector<std::string>& ov_entrypoint, bool ov_entrypoint_set,
    const std::vector<std::string>& ov_cmd, bool ov_cmd_set) {
    const std::vector<std::string>& ep  = ov_entrypoint_set ? ov_entrypoint : img_entrypoint;
    const std::vector<std::string>& cmd = ov_cmd_set ? ov_cmd : img_cmd;
    std::vector<std::string> args = ep;
    args.insert(args.end(), cmd.begin(), cmd.end());
    return args;
}

long long parse_mem_limit(const std::string& s_) {
    const std::string s = trim(s_);
    if (s.empty()) return 0;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || v < 0) return 0;
    std::string suf;
    while (*end) suf.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*end++))));
    long long mult = 1;
    if (suf.empty() || suf == "b")                                    mult = 1;
    else if (suf == "k" || suf == "kb" || suf == "ki" || suf == "kib") mult = 1024LL;
    else if (suf == "m" || suf == "mb" || suf == "mi" || suf == "mib") mult = 1024LL * 1024;
    else if (suf == "g" || suf == "gb" || suf == "gi" || suf == "gib") mult = 1024LL * 1024 * 1024;
    else return 0;
    return static_cast<long long>(v * static_cast<double>(mult));
}

long long parse_cpu_quota(const std::string& cpus_, long long period) {
    const std::string s = trim(cpus_);
    if (s.empty()) return 0;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || v <= 0) return 0;
    return static_cast<long long>(v * static_cast<double>(period));
}

void resolve_user(const std::string& user, unsigned& uid, unsigned& gid) {
    uid = 0;
    gid = 0;
    const std::string s = trim(user);
    if (s.empty()) return;
    const auto colon = s.find(':');
    const std::string us = (colon == std::string::npos) ? s : s.substr(0, colon);
    const std::string gs = (colon == std::string::npos) ? std::string() : s.substr(colon + 1);
    if (all_digits(us)) uid = static_cast<unsigned>(std::strtoul(us.c_str(), nullptr, 10));
    if (all_digits(gs)) gid = static_cast<unsigned>(std::strtoul(gs.c_str(), nullptr, 10));
}

std::string generate_oci_config(const OciSpec& spec) {
    json j;
    j["ociVersion"] = "1.0.0";

    json proc;
    proc["terminal"] = false;
    proc["user"] = {{"uid", spec.uid}, {"gid", spec.gid}};
    proc["args"] = spec.args;

    std::vector<std::string> env = spec.env;
    if (env.empty())
        env.push_back("PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
    proc["env"] = env;
    proc["cwd"] = spec.cwd.empty() ? "/" : spec.cwd;

    // Docker-like default capability set (non-privileged container).
    const json caps = json::array({
        "CAP_CHOWN", "CAP_DAC_OVERRIDE", "CAP_FSETID", "CAP_FOWNER", "CAP_MKNOD",
        "CAP_NET_RAW", "CAP_SETGID", "CAP_SETUID", "CAP_SETFCAP", "CAP_SETPCAP",
        "CAP_NET_BIND_SERVICE", "CAP_SYS_CHROOT", "CAP_KILL", "CAP_AUDIT_WRITE"});
    proc["capabilities"] = {{"bounding", caps}, {"effective", caps}, {"permitted", caps}};
    proc["noNewPrivileges"] = true;
    proc["rlimits"] = json::array({{{"type", "RLIMIT_NOFILE"}, {"hard", 1024}, {"soft", 1024}}});
    j["process"] = proc;

    j["root"] = {{"path", "rootfs"}, {"readonly", false}};
    j["hostname"] = spec.hostname;

    json mounts = json::array();
    mounts.push_back({{"destination", "/proc"}, {"type", "proc"}, {"source", "proc"}});
    mounts.push_back({{"destination", "/dev"}, {"type", "tmpfs"}, {"source", "tmpfs"},
                      {"options", json::array({"nosuid", "strictatime", "mode=755", "size=65536k"})}});
    mounts.push_back({{"destination", "/dev/pts"}, {"type", "devpts"}, {"source", "devpts"},
                      {"options", json::array({"nosuid", "noexec", "newinstance", "ptmxmode=0666", "mode=0620"})}});
    mounts.push_back({{"destination", "/dev/shm"}, {"type", "tmpfs"}, {"source", "shm"},
                      {"options", json::array({"nosuid", "noexec", "nodev", "mode=1777", "size=65536k"})}});
    mounts.push_back({{"destination", "/dev/mqueue"}, {"type", "mqueue"}, {"source", "mqueue"},
                      {"options", json::array({"nosuid", "noexec", "nodev"})}});
    mounts.push_back({{"destination", "/sys"}, {"type", "sysfs"}, {"source", "sysfs"},
                      {"options", json::array({"nosuid", "noexec", "nodev", "ro"})}});
    if (spec.bind_resolv_conf)
        mounts.push_back({{"destination", "/etc/resolv.conf"}, {"type", "bind"},
                          {"source", "/etc/resolv.conf"},
                          {"options", json::array({"rbind", "ro"})}});
    j["mounts"] = mounts;

    json lin;
    // Host networking (default) omits the network namespace → the container
    // shares the host netns (no IP of its own). Bridge mode adds its own
    // network namespace; net_bridge then wires a veth + IP into it.
    json ns = json::array({{{"type", "pid"}}, {{"type", "ipc"}},
                           {{"type", "uts"}}, {{"type", "mount"}}});
    if (!spec.host_network) ns.push_back({{"type", "network"}});
    lin["namespaces"] = ns;
    lin["maskedPaths"] = json::array({
        "/proc/acpi", "/proc/asound", "/proc/kcore", "/proc/keys", "/proc/latency_stats",
        "/proc/timer_list", "/proc/timer_stats", "/proc/sched_debug", "/proc/scsi", "/sys/firmware"});
    lin["readonlyPaths"] = json::array({
        "/proc/bus", "/proc/fs", "/proc/irq", "/proc/sys", "/proc/sysrq-trigger"});

    json resources;
    if (spec.mem_limit_bytes > 0) resources["memory"] = {{"limit", spec.mem_limit_bytes}};
    if (spec.cpu_quota > 0)       resources["cpu"] = {{"quota", spec.cpu_quota}, {"period", spec.cpu_period}};
    if (!resources.empty())       lin["resources"] = resources;
    j["linux"] = lin;

    return j.dump(2);
}

} // namespace containers
