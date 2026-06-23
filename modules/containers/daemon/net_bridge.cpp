#include "net_bridge.hpp"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <ace/Log_Msg.h>
#include <ace/OS_NS_unistd.h>
#include <ace/Process.h>

#include "container_net.hpp"

namespace containers {

namespace {

std::string resolve(const char* name, std::initializer_list<const char*> paths) {
    for (const char* p : paths)
        if (ACE_OS::access(p, X_OK) == 0) return p;
    return name;   // fall back to PATH (execvp)
}

const std::string& ip_tool() {
    static const std::string p = resolve("ip", {"/sbin/ip", "/usr/sbin/ip", "/bin/ip", "/usr/bin/ip"});
    return p;
}
const std::string& nft_tool() {
    static const std::string p = resolve("nft", {"/usr/sbin/nft", "/sbin/nft", "/usr/bin/nft", "/bin/nft"});
    return p;
}

std::string make_temp(const char* tag) {
    std::string tmpl = std::string("/tmp/iot-net-") + tag + "-XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    int fd = ::mkstemp(buf.data());
    if (fd < 0) return {};
    ::close(fd);
    return std::string(buf.data());
}

std::string read_file(const std::string& path) {
    std::string out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    char b[1024];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) out.append(b, n);
    std::fclose(f);
    return out;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\n' || s[a] == '\r' || s[a] == '\t')) ++a;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\n' || s[b-1] == '\r' || s[b-1] == '\t')) --b;
    return s.substr(a, b - a);
}

// Run a command, capturing stderr into `err` on failure. Returns exit code.
int run(const std::vector<std::string>& args, std::string& err) {
    const std::string err_path = make_temp("err");
    std::vector<char*> argv;
    for (auto& s : const_cast<std::vector<std::string>&>(args)) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    ACE_Process_Options opts;
    opts.command_line(argv.data());
    int devnull = ::open("/dev/null", O_RDWR);
    int errfd   = err_path.empty() ? -1 : ::open(err_path.c_str(), O_WRONLY | O_TRUNC);
    if (devnull >= 0 && errfd >= 0) opts.set_handles(devnull, devnull, errfd);

    ACE_Process proc;
    pid_t pid = proc.spawn(opts);
    if (devnull >= 0) ::close(devnull);
    if (errfd   >= 0) ::close(errfd);
    if (pid == ACE_INVALID_PID) {
        if (!err_path.empty()) ::unlink(err_path.c_str());
        err = "spawn failed";
        return -1;
    }
    ACE_exitcode status = 0;
    proc.wait(&status);
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code != 0 && !err_path.empty()) err = trim(read_file(err_path));
    if (!err_path.empty()) ::unlink(err_path.c_str());
    return code;
}

// Run, ignoring the result (idempotent setup / best-effort cleanup).
void run_ok(const std::vector<std::string>& args) {
    std::string ignore;
    run(args, ignore);
}

void write_proc(const char* path, const char* val) {
    FILE* f = std::fopen(path, "w");
    if (f) { std::fputs(val, f); std::fclose(f); }
}

} // namespace

BridgeNet bridge_up(long container_pid, const std::string& subnet_cidr,
                    const std::string& id) {
    BridgeNet res;
    const NetPlan plan = plan_bridge_net(subnet_cidr, kBridgeName);
    if (!plan.ok) { res.error = "invalid bridge subnet: " + subnet_cidr; return res; }

    const std::string& ip = ip_tool();
    const std::string host_veth = "vh-" + id;   // host end (≤15 chars)
    const std::string peer_veth = "vp-" + id;   // container end
    const std::string netns     = "ctr-" + id;  // ip-netns name for the bind mount
    const std::string pfx       = std::to_string(plan.prefix);
    std::string err;

    // 1. Host bridge (create if absent) + gateway IP + up.
    if (run({ip, "link", "show", kBridgeName}, err) != 0)
        run_ok({ip, "link", "add", kBridgeName, "type", "bridge"});
    run_ok({ip, "addr", "add", plan.gateway + "/" + pfx, "dev", kBridgeName});  // EEXIST ok
    run_ok({ip, "link", "set", kBridgeName, "up"});

    // 2. IPv4 forwarding.
    write_proc("/proc/sys/net/ipv4/ip_forward", "1\n");

    // 3. Scoped masquerade nft table (separate from net-router's iot_router).
    const std::string ruleset = nft_container_ruleset(plan.cidr, kBridgeName);
    const std::string nftfile = make_temp("nft");
    if (!nftfile.empty()) { write_proc(nftfile.c_str(), ruleset.c_str()); }
    if (nftfile.empty() || run({nft_tool(), "-f", nftfile}, err) != 0) {
        if (!nftfile.empty()) ::unlink(nftfile.c_str());
        res.error = "nft masquerade failed: " + err;
        return res;
    }
    ::unlink(nftfile.c_str());

    // 4. veth pair → bridge, peer into the container netns as eth0.
    run_ok({ip, "link", "del", host_veth});   // clear any stale veth
    run_ok({ip, "netns", "del", netns});
    if (run({ip, "link", "add", host_veth, "type", "veth", "peer", "name", peer_veth}, err) != 0) {
        res.error = "veth create failed: " + err; return res;
    }
    run_ok({ip, "link", "set", host_veth, "master", kBridgeName});
    run_ok({ip, "link", "set", host_veth, "up"});

    if (run({ip, "netns", "attach", netns, std::to_string(container_pid)}, err) != 0) {
        run_ok({ip, "link", "del", host_veth});
        res.error = "netns attach failed: " + err; return res;
    }
    if (run({ip, "link", "set", peer_veth, "netns", netns}, err) != 0) {
        run_ok({ip, "netns", "del", netns});
        run_ok({ip, "link", "del", host_veth});
        res.error = "veth move failed: " + err; return res;
    }
    run_ok({ip, "-n", netns, "link", "set", peer_veth, "name", "eth0"});
    run_ok({ip, "-n", netns, "addr", "add", plan.container_ip + "/" + pfx, "dev", "eth0"});
    run_ok({ip, "-n", netns, "link", "set", "eth0", "up"});
    run_ok({ip, "-n", netns, "link", "set", "lo", "up"});
    run_ok({ip, "-n", netns, "route", "add", "default", "via", plan.gateway});
    run_ok({ip, "netns", "del", netns});   // drop the bind mount (netns lives via the pid)

    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [ctr] bridge: %C → %C (gw %C)\n"),
               id.c_str(), plan.container_ip.c_str(), plan.gateway.c_str()));
    res.ok = true;
    res.ip = plan.container_ip;
    res.gateway = plan.gateway;
    return res;
}

void bridge_down(const std::string& id) {
    run_ok({ip_tool(), "link", "del", "vh-" + id});
    run_ok({ip_tool(), "netns", "del", "ctr-" + id});
}

} // namespace containers
