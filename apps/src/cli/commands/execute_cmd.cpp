#include "cli/commands/execute_cmd.hpp"

#include <iostream>

#include "cli/coap_dispatch.hpp"
#include "cli/command_context.hpp"

Command::Result ExecuteCmd::execute(CommandContext& ctx,
                                     const std::unordered_map<std::string, std::string>& kv) {
    auto pit = kv.find("path");
    if (pit == kv.end() || pit->second.empty()) {
        std::cout << "execute: missing path=\n";
        return Command::Result::InvalidArgs;
    }
    std::vector<std::string> uris    = cli::split(pit->second, '/');
    std::vector<std::string> payload;
    auto ait = kv.find("args");
    if (ait != kv.end() && !ait->second.empty()) {
        payload.push_back(ait->second);
    }
    // Content-Format 0 = text/plain (LwM2M §6.5.4 Execute args).
    return cli::dispatch(ctx, uris, /*queries=*/{}, payload, /*cf=*/0, /*POST=*/2);
}
