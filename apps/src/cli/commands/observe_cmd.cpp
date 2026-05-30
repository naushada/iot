#include "cli/commands/observe_cmd.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>

#include "cli/coap_dispatch.hpp"
#include "cli/command_context.hpp"

Command::Result ObserveCmd::execute(CommandContext& ctx,
                                     const std::unordered_map<std::string, std::string>& kv) {
    auto pit = kv.find("path");
    if (pit == kv.end() || pit->second.empty()) {
        std::cout << "observe: missing path=\n";
        return Command::Result::InvalidArgs;
    }
    bool cancel = false;
    auto cit = kv.find("cancel");
    if (cit != kv.end()) {
        std::string v = cit->second;
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        cancel = (v == "true" || v == "1");
    }
    // The current CoAPAdapter::serialise does not wire up the Observe
    // option directly; it is treated as a regular GET here. The
    // bookkeeping side (DmServer Observe registration) is driven by the
    // server's response routing, not by an option on the wire from the
    // CLI. The cancel= toggle is accepted for forward-compat but does
    // not currently differentiate the frame — see follow-up in
    // apps/docs/leshan-interop.md when the Observe option lands.
    (void)cancel;

    std::vector<std::string> uris = cli::split(pit->second, '/');
    return cli::dispatch(ctx, uris, /*queries=*/{}, /*payload=*/{}, /*cf=*/0, /*GET=*/1);
}
