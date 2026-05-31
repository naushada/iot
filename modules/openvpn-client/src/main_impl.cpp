#include "openvpn_client/client.hpp"

#include <sstream>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <ace/Log_Msg.h>

#include "data_store/client.hpp"
#include "data_store/value.hpp"

namespace openvpn_client {

namespace {

/// Stringify a Value for a single ACE_DEBUG log line. Keeps the
/// dump human-readable without dragging in nlohmann::json.
std::string value_to_display(const data_store::Value& v) {
    return std::visit([](auto&& a) -> std::string {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, std::monostate>) return "(null)";
        else if constexpr (std::is_same_v<T, bool>)
            return a ? "true" : "false";
        else if constexpr (std::is_same_v<T, std::string>) return a;
        else if constexpr (std::is_same_v<T, double>) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", a);
            return buf;
        } else {
            return std::to_string(a);
        }
    }, v);
}

/// All vpn.* keys the schema (L12/D1) declares. Order matches the
/// schema file's read-then-write grouping so the dump reads naturally.
const std::vector<std::string>& known_keys() {
    static const std::vector<std::string> ks = {
        "vpn.remote.host",
        "vpn.remote.port",
        "vpn.remote.proto",
        "vpn.cert.path",
        "vpn.key.path",
        "vpn.ca.path",
        "vpn.cipher",
        "vpn.dev",
        "vpn.mgmt.port",
        "vpn.state",
        "vpn.assigned.ip",
        "vpn.assigned.gateway",
        "vpn.assigned.netmask",
        "vpn.assigned.dns",
        "vpn.pid",
        "vpn.exit_code",
    };
    return ks;
}

} // namespace

Status v0_dump_vpn_keys(const std::string& socketPath) {
    data_store::Client cli;
    auto cs = cli.connect(socketPath);
    if (!cs.ok) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [ovpn:%t] %M %N:%l data-store connect "
                            "failed: %C\n"),
                   cs.err.c_str()));
        Status s;
        s.ok = false;
        s.code = cs.code;
        s.err = cs.err;
        return s;
    }

    std::vector<data_store::Client::GetResult> got;
    auto gs = cli.get(known_keys(), got);
    if (!gs.ok) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [ovpn:%t] %M %N:%l get(vpn.*) failed: %C\n"),
                   gs.err.c_str()));
        Status s;
        s.ok = false;
        s.code = gs.code;
        s.err = gs.err;
        return s;
    }

    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [ovpn:%t] %M %N:%l vpn.* snapshot "
                        "(%u keys) from %C:\n"),
               static_cast<unsigned>(got.size()),
               socketPath.empty() ? "(default socket)" : socketPath.c_str()));
    for (const auto& r : got) {
        if (!r.has_value) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D [ovpn:%t] %M %N:%l   %C = <unset>\n"),
                       r.key.c_str()));
        } else {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D [ovpn:%t] %M %N:%l   %C = %C\n"),
                       r.key.c_str(), value_to_display(r.value).c_str()));
        }
    }
    return {};
}

} // namespace openvpn_client
