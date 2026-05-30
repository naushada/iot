#ifndef __cli_commands_register_cmd_hpp__
#define __cli_commands_register_cmd_hpp__

#include "cli/command.hpp"

/// `register ep=urn:dev:foo lt=86400 [b=U] [lwm2m=1.1]`
///   → POST /rd?ep=…&lt=…&lwm2m=…&b=…  with a link-format payload
/// listing the canonical OMA Object set (/3/0, /4/0, /6/0, /7/0).
class RegisterCmd : public Command {
public:
    std::string name() const override { return "register"; }
    std::string usage() const override {
        return "register ep=<endpoint> [lt=<seconds>] [b=U] [lwm2m=1.1]";
    }
    std::vector<std::string> args() const override {
        return {"ep=", "lt=", "b=", "lwm2m="};
    }
    Result execute(CommandContext& ctx,
                   const std::unordered_map<std::string, std::string>& kv) override;
};

#endif /* __cli_commands_register_cmd_hpp__ */
