#ifndef __cli_commands_delete_cmd_hpp__
#define __cli_commands_delete_cmd_hpp__

#include "cli/command.hpp"

/// `delete path=/3/0` → DELETE on the LwM2M object instance.
class DeleteCmd : public Command {
public:
    std::string name() const override { return "delete"; }
    std::string usage() const override { return "delete path=/<obj>/<inst>"; }
    std::vector<std::string> args() const override { return {"path="}; }
    Result execute(CommandContext& ctx,
                   const std::unordered_map<std::string, std::string>& kv) override;
};

#endif /* __cli_commands_delete_cmd_hpp__ */
