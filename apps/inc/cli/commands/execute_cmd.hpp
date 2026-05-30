#ifndef __cli_commands_execute_cmd_hpp__
#define __cli_commands_execute_cmd_hpp__

#include "cli/command.hpp"

/// `execute path=/3/0/4 [args=<text>]` → POST with the optional
/// arguments string as text/plain (RFC-7252 §5.5.1.1 / LwM2M §6.5.4).
class ExecuteCmd : public Command {
public:
    std::string name() const override { return "execute"; }
    std::string usage() const override {
        return "execute path=/<obj>/<inst>/<res> [args=<text>]";
    }
    std::vector<std::string> args() const override { return {"path=", "args="}; }
    Result execute(CommandContext& ctx,
                   const std::unordered_map<std::string, std::string>& kv) override;
};

#endif /* __cli_commands_execute_cmd_hpp__ */
