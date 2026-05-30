#include "cli/commands/push_cmd.hpp"

#include <iostream>

#include "cli/coap_dispatch.hpp"
#include "cli/command_context.hpp"

Command::Result PushCmd::execute(CommandContext& ctx,
                                  const std::unordered_map<std::string, std::string>& kv) {
    auto it = kv.find("ep");
    if (it == kv.end() || it->second.empty()) {
        std::cout << "push: missing ep=\n";
        return Command::Result::InvalidArgs;
    }
    std::uint16_t cf = 0;
    auto cfit = kv.find("content-format");
    if (cfit != kv.end()) {
        try { cf = static_cast<std::uint16_t>(std::stoi(cfit->second)); }
        catch (...) { cf = 0; }
    }
    return cli::dispatch(ctx, /*uris=*/{"push"}, /*queries=*/{"ep=" + it->second},
                         cli::build_payload(kv, ctx), cf, /*POST=*/2);
}
