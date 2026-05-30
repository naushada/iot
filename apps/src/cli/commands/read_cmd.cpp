#include "cli/commands/read_cmd.hpp"

#include <iostream>

#include "cli/coap_dispatch.hpp"
#include "cli/command_context.hpp"

Command::Result ReadCmd::execute(CommandContext& ctx,
                                  const std::unordered_map<std::string, std::string>& kv) {
    auto it = kv.find("path");
    if (it == kv.end() || it->second.empty()) {
        std::cout << "read: missing path=\n";
        return Command::Result::InvalidArgs;
    }
    std::vector<std::string> uris = cli::split(it->second, '/');
    return cli::dispatch(ctx, uris, /*queries=*/{}, /*payload=*/{}, /*cf=*/0, /*GET=*/1);
}
