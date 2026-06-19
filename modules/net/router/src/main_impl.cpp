#include "router.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <ace/Log_Msg.h>

#include "data_store/client.hpp"
#include "data_store/value.hpp"

namespace net_router {

namespace {

/// All net.* keys the schema (L13/D1) declares — read keys first,
/// then write keys, matching the schema file ordering for an
/// easily-eyeballed dump.
const std::vector<std::string>& known_keys() {
    static const std::vector<std::string> ks = {
        // Reads
        "net.tun.dev",
        "net.lwm2m.target.ip",
        "net.lwm2m.target.port",
        "net.iface.priority",
        "net.iface.eth.name",
        "net.iface.wifi.name",
        "net.iface.cellular.name",
        "net.custom.rules",
        "net.poll.interval.sec",
        // Writes (will be unset on first boot)
        "net.state",
        "net.tun.ip",
        "net.tun.gateway",
        "net.iface.active",
        "net.iface.active.ip",
        "net.rules.applied.count",
        "net.last.apply.unix",
    };
    return ks;
}

} // namespace

Status v0_dump_net_keys(const std::string& socketPath) {
    data_store::Client cli;
    auto cs = cli.connect(socketPath);
    if (!cs.ok) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [netr:%t] %M %N:%l data-store connect "
                            "failed: %C\n"),
                   cs.err.c_str()));
        Status s; s.ok = false; s.code = cs.code; s.err = cs.err;
        return s;
    }

    std::vector<data_store::Client::GetResult> got;
    auto gs = cli.get(known_keys(), got);
    if (!gs.ok) {
        ACE_ERROR((LM_ERROR,
                   ACE_TEXT("%D [netr:%t] %M %N:%l get(net.*) failed: %C\n"),
                   gs.err.c_str()));
        Status s; s.ok = false; s.code = gs.code; s.err = gs.err;
        return s;
    }

    ACE_DEBUG((LM_INFO,
               ACE_TEXT("%D [netr:%t] %M %N:%l net.* snapshot (%u keys) "
                        "from %C:\n"),
               static_cast<unsigned>(got.size()),
               socketPath.empty() ? "(default socket)" : socketPath.c_str()));
    for (const auto& r : got) {
        if (!r.has_value) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D [netr:%t] %M %N:%l   %C = (unset)\n"),
                       r.key.c_str()));
            continue;
        }
        // Render via the typed accessors lifted into value.hpp in
        // PR #37 — same pattern openvpn-client's dump uses.
        if (auto v = data_store::to_string(r.value)) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D [netr:%t] %M %N:%l   %C = %C\n"),
                       r.key.c_str(), v->c_str()));
        } else if (auto v = data_store::to_uint32(r.value)) {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D [netr:%t] %M %N:%l   %C = %u\n"),
                       r.key.c_str(), static_cast<unsigned>(*v)));
        } else {
            ACE_DEBUG((LM_INFO,
                       ACE_TEXT("%D [netr:%t] %M %N:%l   %C = (unrecognised type)\n"),
                       r.key.c_str()));
        }
    }

    // Bring-up gate: net.lwm2m.target.ip is the only required read
    // key (the rest have schema defaults). Mirror the openvpn-client
    // missing-required diagnostic so the operator sees what to fix.
    bool target_ip_set = false;
    for (const auto& r : got) {
        if (r.key == "net.lwm2m.target.ip" && r.has_value) {
            target_ip_set = true;
            break;
        }
    }
    if (!target_ip_set) {
        ACE_DEBUG((LM_WARNING,
                   ACE_TEXT("%D [netr:%t] %M %N:%l required key missing: "
                            "net.lwm2m.target.ip — daemon mode (D6) will "
                            "refuse to start\n")));
    } else {
        ACE_DEBUG((LM_INFO,
                   ACE_TEXT("%D [netr:%t] %M %N:%l required keys present; "
                            "ready for D3..D6 lifecycle\n")));
    }
    return {};
}

} // namespace net_router
