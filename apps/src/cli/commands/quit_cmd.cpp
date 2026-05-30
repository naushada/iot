#include "cli/commands/quit_cmd.hpp"

#include "cli/command_context.hpp"

Command::Result QuitCmd::execute(CommandContext& ctx,
                                  const std::unordered_map<std::string, std::string>& /*kv*/) {
    ctx.should_quit = true;
    return Command::Result::Quit;
}
