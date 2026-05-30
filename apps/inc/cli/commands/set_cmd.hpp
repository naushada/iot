#ifndef __cli_commands_set_cmd_hpp__
#define __cli_commands_set_cmd_hpp__

#include "cli/command.hpp"

/// `set ep=<endpoint> [data=<json>] [file=<path>] [content-format=N]`
///   → POST /set?ep=<endpoint>
/// Data-plane WRITE of one or more data points (companion of `get`,
/// distinct from the LwM2M-level `write`).
class SetCmd : public Command {
public:
    std::string name() const override { return "set"; }
    std::string usage() const override {
        return "set ep=<endpoint> [data=<json>] [file=<path>] [content-format=N]";
    }
    std::vector<std::string> args() const override {
        return {"ep=", "data=", "file=", "content-format="};
    }
    Result execute(CommandContext& ctx,
                   const std::unordered_map<std::string, std::string>& kv) override;
};

#endif /* __cli_commands_set_cmd_hpp__ */
