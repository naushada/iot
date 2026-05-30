#include "cli/commands/write_cmd.hpp"

#include <iostream>

#include "cli/coap_dispatch.hpp"
#include "cli/command_context.hpp"

Command::Result WriteCmd::execute(CommandContext& ctx,
                                   const std::unordered_map<std::string, std::string>& kv) {
    auto pit = kv.find("path");
    auto vit = kv.find("value");
    if (pit == kv.end() || pit->second.empty() ||
        vit == kv.end() || vit->second.empty()) {
        std::cout << "write: need path= and value=\n";
        return Command::Result::InvalidArgs;
    }
    std::vector<std::string> uris    = cli::split(pit->second, '/');
    std::vector<std::string> payload = {vit->second};
    // Content-Format 0 = text/plain (LwM2M §6.3.4 single resource).
    return cli::dispatch(ctx, uris, /*queries=*/{}, payload, /*cf=*/0, /*PUT=*/3);
}
