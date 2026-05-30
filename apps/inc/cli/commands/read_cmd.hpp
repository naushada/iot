#ifndef __cli_commands_read_cmd_hpp__
#define __cli_commands_read_cmd_hpp__

#include "cli/command.hpp"

/// `read path=/3/0[/0]` → GET on the LwM2M resource path.
class ReadCmd : public Command {
public:
    std::string name() const override { return "read"; }
    std::string usage() const override {
        return "read path=/<obj>/<inst>[/<res>]";
    }
    std::vector<std::string> args() const override { return {"path="}; }
    Result execute(CommandContext& ctx,
                   const std::unordered_map<std::string, std::string>& kv) override;
};

#endif /* __cli_commands_read_cmd_hpp__ */
