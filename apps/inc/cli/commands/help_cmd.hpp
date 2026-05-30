#ifndef __cli_commands_help_cmd_hpp__
#define __cli_commands_help_cmd_hpp__

#include "cli/command.hpp"

class HelpCmd : public Command {
public:
    std::string name() const override { return "help"; }
    std::string usage() const override {
        return "help [<command>]    list commands or print usage for one";
    }
    std::vector<std::string> args() const override { return {}; }
    Result execute(CommandContext& ctx,
                   const std::unordered_map<std::string, std::string>& kv) override;
};

#endif /* __cli_commands_help_cmd_hpp__ */
