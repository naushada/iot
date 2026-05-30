#ifndef __cli_commands_push_cmd_hpp__
#define __cli_commands_push_cmd_hpp__

#include "cli/command.hpp"

/// `push ep=<endpoint> [data=<json>] [file=<path>] [content-format=N]`
///   → POST /push?ep=<endpoint>
/// Data-plane push toward the server (custom OMA-style push plane,
/// see project memory project_lwm2m_push_plane.md).
class PushCmd : public Command {
public:
    std::string name() const override { return "push"; }
    std::string usage() const override {
        return "push ep=<endpoint> [data=<json>] [file=<path>] [content-format=N]";
    }
    std::vector<std::string> args() const override {
        return {"ep=", "data=", "file=", "content-format="};
    }
    Result execute(CommandContext& ctx,
                   const std::unordered_map<std::string, std::string>& kv) override;
};

#endif /* __cli_commands_push_cmd_hpp__ */
