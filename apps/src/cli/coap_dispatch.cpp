#include "cli/coap_dispatch.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>

#include "app.hpp"
#include "cli/command_context.hpp"
#include "coap_adapter.hpp"
#include "udp_adapter.hpp"

namespace cli {

Command::Result dispatch(CommandContext& ctx,
                         const std::vector<std::string>& uris,
                         const std::vector<std::string>& queries,
                         const std::vector<std::string>& payload,
                         std::uint16_t                   cf,
                         std::uint8_t                    method) {
    CoAPAdapter coap;
    std::vector<std::string> frames;
    if (!coap.serialise(uris, queries, payload, cf, method, frames) || frames.empty()) {
        std::cout << "cli/coap_dispatch: serialise failed (method=" << static_cast<int>(method)
                  << ", uris=" << uris.size() << ")\n";
        return Command::Result::DispatchFailed;
    }

    auto& udp = ctx.app->udpAdapter();
    // Try ServiceContexts in priority order: client-side first
    // (LwM2MClient is what the legacy code used), then DM client,
    // then DM server, then bootstrap server. The fallback chain lets
    // the same `post` command work whether the binary is a client or
    // a server. LwM2M-level commands (read/write/observe) on a pure
    // server still need to know which registered client to target —
    // that's a separate follow-up; for now they ship through whatever
    // ServiceContext the chain finds.
    static const UDPAdapter::ServiceType_t kOrder[] = {
        UDPAdapter::ServiceType_t::LwM2MClient,
        UDPAdapter::ServiceType_t::DeviceMgmtClient,
        UDPAdapter::ServiceType_t::DeviceMgmtServer,
        UDPAdapter::ServiceType_t::BootsstrapServer,
    };
    auto it = udp->services().end();
    for (auto svc : kOrder) {
        it = std::find_if(udp->services().begin(), udp->services().end(),
                          [svc](auto& ent) { return ent.second->service() == svc; });
        if (it != udp->services().end()) break;
    }
    if (it == udp->services().end()) {
        std::cout << "cli/coap_dispatch: no ServiceContext registered\n";
        return Command::Result::DispatchFailed;
    }

    bool any_ok = false;
    auto svc = it->second->service();
    for (auto& frame : frames) {
        if (udp->tx(frame, svc) < 0) {
            std::cout << "cli/coap_dispatch: enqueue tx failed for svc=" << svc << "\n";
            continue;
        }
        any_ok = true;
    }
    return any_ok ? Command::Result::Ok : Command::Result::DispatchFailed;
}

std::vector<std::string> build_payload(
        const std::unordered_map<std::string, std::string>& kv,
        CommandContext& ctx) {
    auto it_data = kv.find("data");
    auto it_file = kv.find("file");
    std::vector<std::string> payload;
    if (it_file != kv.end() && !it_file->second.empty()) {
        CoAPAdapter coap;
        std::string content = ctx.cbor.getJson(it_file->second);
        coap.buildRequest(content, payload);
    } else if (it_data != kv.end() && !it_data->second.empty() &&
               it_data->second.length() <= 1024) {
        std::string encoded;
        ctx.cbor.json2cbor(it_data->second, encoded);
        if (!encoded.empty()) payload.push_back(std::move(encoded));
    }
    return payload;
}

std::vector<std::string> split(const std::string& in, char delim) {
    std::vector<std::string> out;
    if (in.empty()) return out;

    std::istringstream  iss(in);
    std::ostringstream  buf;
    while (!iss.get(*buf.rdbuf(), delim).eof()) {
        if (buf.str().empty()) {
            iss.get();
        } else {
            if (buf.str().at(0) == '"' && buf.str().substr(1).length() > 0) {
                out.push_back(buf.str().substr(1));
            } else if (buf.str().at(0) != '"') {
                out.push_back(buf.str());
            }
            buf.str("");
            iss.get();
        }
    }

    if (!buf.str().empty()) {
        std::string tail = buf.str();
        if (tail.at(0) == '"') tail = tail.substr(1);
        std::istringstream iss2(tail);
        std::ostringstream key;
        iss2.get(*key.rdbuf(), '"');
        out.push_back(key.str());
    }
    return out;
}

} // namespace cli
