#ifndef __cli_commands_bootstrap_cmd_hpp__
#define __cli_commands_bootstrap_cmd_hpp__

#include "cli/command.hpp"

/// `bootstrap ep=urn:dev:foo` → POST /bs?ep=…
/// Drops to the bootstrap-server URI at the configured local port.
class BootstrapCmd : public Command {
public:
    std::string name() const override { return "bootstrap"; }
    std::string usage() const override { return "bootstrap ep=<endpoint>"; }
    std::vector<std::string> args() const override { return {"ep="}; }
    Result execute(CommandContext& ctx,
                   const std::unordered_map<std::string, std::string>& kv) override;
};

#endif /* __cli_commands_bootstrap_cmd_hpp__ */
