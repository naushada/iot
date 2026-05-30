#include "cli/commands/bootstrap_cmd.hpp"

#include <iostream>

#include "cli/coap_dispatch.hpp"
#include "cli/command_context.hpp"

Command::Result BootstrapCmd::execute(CommandContext& ctx,
                                       const std::unordered_map<std::string, std::string>& kv) {
    auto it = kv.find("ep");
    if (it == kv.end() || it->second.empty()) {
        std::cout << "bootstrap: missing ep=\n";
        return Command::Result::InvalidArgs;
    }
    const std::vector<std::string> uris    = {"bs"};
    const std::vector<std::string> queries = {"ep=" + it->second};
    return cli::dispatch(ctx, uris, queries, /*payload=*/{}, /*cf=*/0, /*POST=*/2);
}
