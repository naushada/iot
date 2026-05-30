#ifndef __cli_command_registry_hpp__
#define __cli_command_registry_hpp__

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "cli/command.hpp"

/// Owns every Command instance for the lifetime of the REPL. Lookup is
/// O(1) by name; iteration order is insertion order so `help` lists
/// commands in the same order as build_default() declares them.
class CommandRegistry {
public:
    CommandRegistry() = default;
    CommandRegistry(const CommandRegistry&) = delete;
    CommandRegistry& operator=(const CommandRegistry&) = delete;
    // Need an explicit move ctor — declaring the copy ctor as deleted
    // suppresses the implicit move, but build_default() returns by value.
    CommandRegistry(CommandRegistry&&) = default;
    CommandRegistry& operator=(CommandRegistry&&) = default;

    void add(std::unique_ptr<Command> cmd);

    Command*       find(const std::string& name);
    const Command* find(const std::string& name) const;

    /// Ordered names for completion and help listing.
    const std::vector<std::string>& names() const { return m_order; }

    /// Build the default registry with every command this binary
    /// supports (low-level post + nine LwM2M-level / meta commands).
    static CommandRegistry build_default();

private:
    std::unordered_map<std::string, std::unique_ptr<Command>> m_by_name;
    std::vector<std::string>                                  m_order;
};

#endif /* __cli_command_registry_hpp__ */
