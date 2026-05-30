#ifndef __cli_command_context_hpp__
#define __cli_command_context_hpp__

#include <memory>

class App;
class CBORAdapter;
class CommandRegistry;

/// Per-invocation state handed to Command::execute(). Owned by
/// Readline and rebuilt on the stack for each command. Keeps the
/// command surface decoupled from Readline itself so commands compile
/// without pulling in GNU readline headers.
struct CommandContext {
    std::shared_ptr<App>& app;
    CBORAdapter&          cbor;
    bool&                 should_quit;     ///< quit_cmd sets this to true
    const CommandRegistry& registry;       ///< help_cmd walks this
};

#endif /* __cli_command_context_hpp__ */
