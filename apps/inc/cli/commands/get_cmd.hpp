#ifndef __cli_commands_get_cmd_hpp__
#define __cli_commands_get_cmd_hpp__

#include "cli/command.hpp"

/// `get ep=<endpoint> [data=<json>] [file=<path>] [content-format=N]`
///   → POST /get?ep=<endpoint>
/// Data-plane READ of one or more data points (companion of `set`,
/// distinct from the LwM2M-level `read`). The request payload, when
/// supplied, selects which keys to fetch.
class GetCmd : public Command {
public:
    std::string name() const override { return "get"; }
    std::string usage() const override {
        return "get ep=<endpoint> [data=<json>] [file=<path>] [content-format=N]";
    }
    std::vector<std::string> args() const override {
        return {"ep=", "data=", "file=", "content-format="};
    }
    Result execute(CommandContext& ctx,
                   const std::unordered_map<std::string, std::string>& kv) override;
};

#endif /* __cli_commands_get_cmd_hpp__ */
