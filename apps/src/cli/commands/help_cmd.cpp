#include "cli/commands/help_cmd.hpp"

#include <iostream>

#include "cli/command_context.hpp"
#include "cli/command_registry.hpp"

namespace {

std::string find_or(const std::unordered_map<std::string, std::string>& kv,
                    const std::string& key) {
    auto it = kv.find(key);
    return (it == kv.end()) ? std::string() : it->second;
}

} // namespace

Command::Result HelpCmd::execute(CommandContext& ctx,
                                  const std::unordered_map<std::string, std::string>& kv) {
    // `help foo` (single positional arg parsed into the "name=" slot
    // by the legacy line splitter, but commonly typed as just "foo")
    // is supported by checking both keys.
    std::string which = find_or(kv, "name");
    if (which.empty() && !kv.empty()) {
        // pick the first key with empty value — that's the bare token
        for (const auto& p : kv) {
            if (p.second.empty()) { which = p.first; break; }
        }
    }

    if (!which.empty()) {
        const Command* c = ctx.registry.find(which);
        if (c) {
            std::cout << "  " << c->name() << "    " << c->usage() << "\n";
        } else {
            std::cout << "help: no such command: " << which << "\n";
        }
        return Command::Result::Ok;
    }

    for (const std::string& n : ctx.registry.names()) {
        const Command* c = ctx.registry.find(n);
        if (!c) continue;
        std::cout << "  " << c->name();
        for (size_t i = c->name().size(); i < 12; ++i) std::cout << ' ';
        std::cout << c->usage() << "\n";
    }
    return Command::Result::Ok;
}
