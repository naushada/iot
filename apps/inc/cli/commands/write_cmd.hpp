#ifndef __cli_commands_write_cmd_hpp__
#define __cli_commands_write_cmd_hpp__

#include "cli/command.hpp"

/// `write path=/3/0/15 value="Europe/Berlin"` → PUT with the value as
/// text/plain (Content-Format 0). For richer encodings, use the
/// low-level `post` command.
class WriteCmd : public Command {
public:
    std::string name() const override { return "write"; }
    std::string usage() const override {
        return "write path=/<obj>/<inst>/<res> value=<text>";
    }
    std::vector<std::string> args() const override { return {"path=", "value="}; }
    Result execute(CommandContext& ctx,
                   const std::unordered_map<std::string, std::string>& kv) override;
};

#endif /* __cli_commands_write_cmd_hpp__ */
