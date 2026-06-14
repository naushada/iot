#include "device_dnat.hpp"

#include <cstdio>
#include <sstream>
#include <string>

#include <ace/Log_Msg.h>

namespace server {
namespace dnat {

namespace {
constexpr const char* kTable = "iot_cloud_dnat";
} // namespace

std::string format_mapping(const DeviceForward& d, std::uint16_t ui_port) {
    std::ostringstream ss;
    ss << "tcp dport " << d.proxy_port << " -> " << d.tun_ip << ":" << ui_port;
    return ss.str();
}

std::string build_device_dnat_ruleset(const RulesetInput& in) {
    std::ostringstream ss;
    // Scope everything to our own table so re-applying flushes only our
    // rules (never NetworkManager / docker / openvpn-managed chains). We
    // use the `ip` family (IPv4) — the VPN subnet is IPv4.
    //
    // `add table` BEFORE `flush table`: on a fresh netns (every container
    // start) the table doesn't exist yet, and `flush table` on a missing
    // table is a hard error that aborts the whole ruleset — so the DNAT
    // never applied. `add table` is idempotent (creates if absent, no-op if
    // present), guaranteeing the subsequent flush + rebuild always succeed.
    ss << "add table ip " << kTable << "\n";
    ss << "flush table ip " << kTable << "\n";
    ss << "table ip " << kTable << " {\n";

    // ── prerouting (NAT): the per-device DNAT ──────────────────────
    // cloud:<proxy_port> → <tun_ip>:<ui_port>. priority dstnat (-100)
    // so it runs before routing, like the standard nat prerouting hook.
    ss << "    chain prerouting {\n";
    ss << "        type nat hook prerouting priority dstnat; policy accept;\n";
    for (const auto& d : in.devices) {
        if (d.tun_ip.empty() || d.proxy_port == 0) continue;
        ss << "        tcp dport " << d.proxy_port
           << " dnat to " << d.tun_ip << ":" << in.ui_port << "\n";
    }
    ss << "    }\n";

    // ── postrouting (NAT): masquerade toward the tunnel ────────────
    // So the device sees the connection coming from the cloud's tun IP
    // (return path works without the device knowing a route back to the
    // operator). Only masquerade traffic leaving via the tun device.
    ss << "    chain postrouting {\n";
    ss << "        type nat hook postrouting priority srcnat; policy accept;\n";
    if (!in.tun_dev.empty())
        ss << "        oifname \"" << in.tun_dev << "\" masquerade\n";
    ss << "    }\n";

    // ── forward (filter): accept the DNAT'd flow to/from the tunnel ─
    // Default-accept chain; the explicit tun rules make intent clear and
    // keep working even if a stricter policy is layered elsewhere.
    ss << "    chain forward {\n";
    ss << "        type filter hook forward priority filter; policy accept;\n";
    if (!in.tun_dev.empty()) {
        ss << "        oifname \"" << in.tun_dev << "\" accept\n";
        ss << "        iifname \"" << in.tun_dev << "\" accept\n";
    }
    ss << "    }\n";

    ss << "}\n";
    return ss.str();
}

bool apply_ruleset(const std::string& script, const std::string& nft_path) {
    const std::string cmd = nft_path + " -f -";
    FILE* p = ::popen(cmd.c_str(), "w");
    if (!p) {
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("%D cloudd:thread:%t %M %N:%l dnat popen(%C) "
                                   "failed\n"),
                          cmd.c_str()),
                         false);
    }
    ::fwrite(script.data(), 1, script.size(), p);
    const int rc = ::pclose(p);
    if (rc != 0) {
        ACE_ERROR_RETURN((LM_ERROR,
                          ACE_TEXT("%D cloudd:thread:%t %M %N:%l dnat nft apply "
                                   "rc=%d; ruleset:\n%C\n"),
                          rc, script.c_str()),
                         false);
    }
    return true;
}

bool enable_ip_forward() {
    FILE* f = ::fopen("/proc/sys/net/ipv4/ip_forward", "w");
    if (!f) return false;
    const int n = ::fputs("1\n", f);
    ::fclose(f);
    return n >= 0;
}

} // namespace dnat
} // namespace server
