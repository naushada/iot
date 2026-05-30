#ifndef __cli_command_hpp__
#define __cli_command_hpp__

#include <string>
#include <unordered_map>
#include <vector>

class CommandContext;

/// Abstract base for every CLI command. The Readline loop parses an
/// input line into a name and an unordered_map<string,string> of
/// key=value args, then calls Command::execute() on the matching
/// instance. Per-command files live under apps/inc/cli/commands/ and
/// apps/src/cli/commands/.
///
/// All commands are registered in CommandRegistry::build_default()
/// (apps/src/cli/command_registry.cpp). Adding a new command is one
/// header + one .cpp + one line in build_default().
class Command {
public:
    enum class Result {
        Ok,                 ///< command ran, no further action
        InvalidArgs,        ///< args missing/malformed; Readline prints usage
        Quit,               ///< quit the REPL
        DispatchFailed      ///< CoAP serialise or tx failed
    };

    virtual ~Command() = default;

    /// Command name as typed at the prompt. Must be unique within a
    /// CommandRegistry.
    virtual std::string name() const = 0;

    /// One-line usage string shown by `help <name>` and on bad args.
    virtual std::string usage() const = 0;

    /// Argument labels (key=) used for tab-completion. Empty vector for
    /// commands that take no args.
    virtual std::vector<std::string> args() const = 0;

    virtual Result execute(CommandContext& ctx,
                           const std::unordered_map<std::string, std::string>& kv) = 0;
};

#endif /* __cli_command_hpp__ */
