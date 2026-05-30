#include "cli/command_registry.hpp"

#include "cli/commands/bootstrap_cmd.hpp"
#include "cli/commands/delete_cmd.hpp"
#include "cli/commands/exec_cmd.hpp"
#include "cli/commands/execute_cmd.hpp"
#include "cli/commands/get_cmd.hpp"
#include "cli/commands/help_cmd.hpp"
#include "cli/commands/object_alias_cmd.hpp"
#include "cli/commands/observe_cmd.hpp"
#include "cli/commands/post_cmd.hpp"
#include "cli/commands/push_cmd.hpp"
#include "cli/commands/quit_cmd.hpp"
#include "cli/commands/read_cmd.hpp"
#include "cli/commands/register_cmd.hpp"
#include "cli/commands/set_cmd.hpp"
#include "cli/commands/write_cmd.hpp"

void CommandRegistry::add(std::unique_ptr<Command> cmd) {
    if (!cmd) return;
    const std::string n = cmd->name();
    if (m_by_name.find(n) != m_by_name.end()) return;
    m_order.push_back(n);
    m_by_name.emplace(n, std::move(cmd));
}

Command* CommandRegistry::find(const std::string& name) {
    auto it = m_by_name.find(name);
    return (it == m_by_name.end()) ? nullptr : it->second.get();
}

const Command* CommandRegistry::find(const std::string& name) const {
    auto it = m_by_name.find(name);
    return (it == m_by_name.end()) ? nullptr : it->second.get();
}

CommandRegistry CommandRegistry::build_default() {
    CommandRegistry r;
    // LwM2M-level commands first so they top the `help` listing.
    r.add(std::make_unique<BootstrapCmd>());
    r.add(std::make_unique<RegisterCmd>());
    r.add(std::make_unique<ReadCmd>());
    r.add(std::make_unique<WriteCmd>());
    r.add(std::make_unique<ExecuteCmd>());
    r.add(std::make_unique<DeleteCmd>());
    r.add(std::make_unique<ObserveCmd>());
    // Per-OMA-Object name aliases — let users type `device read=0`
    // instead of remembering /3/0/0. iid= defaults to 0; bare command
    // reads the whole instance.
    r.add(std::make_unique<ObjectAliasCmd>("security",       0));
    r.add(std::make_unique<ObjectAliasCmd>("server",         1));
    r.add(std::make_unique<ObjectAliasCmd>("access-control", 2));
    r.add(std::make_unique<ObjectAliasCmd>("device",         3));
    r.add(std::make_unique<ObjectAliasCmd>("connmon",        4));
    r.add(std::make_unique<ObjectAliasCmd>("firmware",       5));
    r.add(std::make_unique<ObjectAliasCmd>("location",       6));
    r.add(std::make_unique<ObjectAliasCmd>("connstat",       7));
    // Data-plane commands (custom OMA-style push plane).
    r.add(std::make_unique<PushCmd>());
    r.add(std::make_unique<SetCmd>());
    r.add(std::make_unique<GetCmd>());
    r.add(std::make_unique<ExecCmd>());
    // Low-level CoAP escape hatch.
    r.add(std::make_unique<PostCmd>());
    // Meta.
    r.add(std::make_unique<HelpCmd>());
    r.add(std::make_unique<QuitCmd>());
    return r;
}
