#include "cli/commands/register_cmd.hpp"

#include <iostream>

#include "cli/coap_dispatch.hpp"
#include "cli/command_context.hpp"

namespace {

std::string find_or(const std::unordered_map<std::string, std::string>& kv,
                    const std::string& k, const std::string& d) {
    auto it = kv.find(k);
    return (it == kv.end()) ? d : it->second;
}

} // namespace

Command::Result RegisterCmd::execute(CommandContext& ctx,
                                      const std::unordered_map<std::string, std::string>& kv) {
    const std::string ep    = find_or(kv, "ep",    "");
    const std::string lt    = find_or(kv, "lt",    "86400");
    const std::string b     = find_or(kv, "b",     "U");
    const std::string lwm2m = find_or(kv, "lwm2m", "1.1");

    if (ep.empty()) {
        std::cout << "register: missing ep=\n";
        return Command::Result::InvalidArgs;
    }

    const std::vector<std::string> uris    = {"rd"};
    const std::vector<std::string> queries = {
        "ep=" + ep,
        "lt=" + lt,
        "lwm2m=" + lwm2m,
        "b=" + b,
    };
    // Default canonical Object set. The legacy interop pcaps
    // (log/L9/nfr-001-coap.pcap) carry the same payload; matching it
    // here keeps the CLI-driven register byte-equivalent to the FSM
    // one.
    const std::string link_format =
        "</>;rt=\"oma.lwm2m\";ct=11542,"
        "</1/0>;ver=\"1.1\","
        "</3/0>,"
        "</4/0>,"
        "</6/0>,"
        "</7/0>";
    const std::vector<std::string> payload = {link_format};

    // Content-Format 40 = application/link-format.
    return cli::dispatch(ctx, uris, queries, payload, /*cf=*/40, /*POST=*/2);
}
