#ifndef __cli_commands_object_alias_cmd_hpp__
#define __cli_commands_object_alias_cmd_hpp__

#include <cstdint>

#include "cli/command.hpp"

/// One instance of this class per OMA canonical Object. Lets the user
/// type `device read=0` instead of `read path=/3/0/0`. The instance
/// stores its Object name + OID; the command picks which low-level
/// operation to dispatch based on which key is present in the args
/// map: read=, write=, exec=, observe=, delete=.
///
/// Layout (all optional except where noted):
///
///   <name>                                 → read /<OID>/<iid>
///   <name> read=<rid>                      → read /<OID>/<iid>/<rid>
///   <name> write=<rid> value=<text>        → write /<OID>/<iid>/<rid>
///   <name> exec=<rid> [args=<text>]        → execute /<OID>/<iid>/<rid>
///   <name> observe=<rid> [cancel=true]     → observe /<OID>/<iid>/<rid>
///   <name> delete[=<rid>]                  → delete /<OID>/<iid>[/<rid>]
///
/// `iid=N` overrides the default instance (0). The eight registered
/// aliases (see CommandRegistry::build_default) cover OMA Objects
/// 0-7 — security, server, access-control, device, connmon, firmware,
/// location, connstat.
class ObjectAliasCmd : public Command {
public:
    ObjectAliasCmd(std::string name, std::uint32_t oid)
      : m_name(std::move(name)), m_oid(oid) {}

    std::string name() const override { return m_name; }
    std::string usage() const override;
    std::vector<std::string> args() const override {
        return {"read=", "write=", "exec=", "observe=", "delete=",
                "iid=", "value=", "args=", "cancel="};
    }
    Result execute(CommandContext& ctx,
                   const std::unordered_map<std::string, std::string>& kv) override;

private:
    std::string   m_name;
    std::uint32_t m_oid;
};

#endif /* __cli_commands_object_alias_cmd_hpp__ */
