#include "cli/command_registry.hpp"

#include "cli/commands/bootstrap_cmd.hpp"
#include "cli/commands/delete_cmd.hpp"
#include "cli/commands/exec_cmd.hpp"
#include "cli/commands/execute_cmd.hpp"
#include "cli/commands/get_cmd.hpp"
#include "cli/commands/help_cmd.hpp"
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
