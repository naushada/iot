#ifndef __cli_commands_observe_cmd_hpp__
#define __cli_commands_observe_cmd_hpp__

#include "cli/command.hpp"

/// `observe path=/3/0/13` → GET /3/0/13 with Observe option = 0.
/// `observe path=/3/0/13 cancel=true` → GET /3/0/13 with Observe = 1.
class ObserveCmd : public Command {
public:
    std::string name() const override { return "observe"; }
    std::string usage() const override {
        return "observe path=/<obj>/<inst>[/<res>] [cancel=true]";
    }
    std::vector<std::string> args() const override { return {"path=", "cancel="}; }
    Result execute(CommandContext& ctx,
                   const std::unordered_map<std::string, std::string>& kv) override;
};

#endif /* __cli_commands_observe_cmd_hpp__ */
