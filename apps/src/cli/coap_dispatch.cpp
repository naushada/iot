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
    auto it = std::find_if(udp->services().begin(), udp->services().end(),
                           [](auto& ent) {
                               return UDPAdapter::ServiceType_t::LwM2MClient == ent.second->service();
                           });
    if (it == udp->services().end()) {
        std::cout << "cli/coap_dispatch: no LwM2MClient ServiceContext registered\n";
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
