#ifndef __cli_commands_exec_cmd_hpp__
#define __cli_commands_exec_cmd_hpp__

#include "cli/command.hpp"

/// `exec ep=<endpoint> [data=<json>] [file=<path>] [content-format=N]`
///   → POST /execute?ep=<endpoint>
/// Data-plane action trigger. Distinct from the LwM2M-level
/// `execute` (which targets /<obj>/<inst>/<res>).
class ExecCmd : public Command {
public:
    std::string name() const override { return "exec"; }
    std::string usage() const override {
        return "exec ep=<endpoint> [data=<json>] [file=<path>] [content-format=N]";
    }
    std::vector<std::string> args() const override {
        return {"ep=", "data=", "file=", "content-format="};
    }
    Result execute(CommandContext& ctx,
                   const std::unordered_map<std::string, std::string>& kv) override;
};

#endif /* __cli_commands_exec_cmd_hpp__ */
