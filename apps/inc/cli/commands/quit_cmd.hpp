#ifndef __cli_commands_quit_cmd_hpp__
#define __cli_commands_quit_cmd_hpp__

#include "cli/command.hpp"

class QuitCmd : public Command {
public:
    std::string name() const override { return "quit"; }
    std::string usage() const override { return "quit    exit the REPL"; }
    std::vector<std::string> args() const override { return {}; }
    Result execute(CommandContext& ctx,
                   const std::unordered_map<std::string, std::string>& kv) override;
};

#endif /* __cli_commands_quit_cmd_hpp__ */
