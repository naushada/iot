#ifndef __readline_hpp__
#define __readline_hpp__

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "app.hpp"
#include "cbor_adapter.hpp"
#include "cli/command_registry.hpp"

extern "C" {
    #include <readline/readline.h>
    #include <readline/history.h>
}

/// Thin shell over GNU readline + libhistory. Owns a CommandRegistry
/// and dispatches each typed line through it. Per-command behaviour
/// lives in apps/{inc,src}/cli/commands/ — see CommandRegistry.
///
/// The free-function tab-completion callbacks rl_attempted_completion_function
/// expects are C-linkage, so they live as friends and reach the
/// registry via a static pointer set in init().
class Readline {
public:
    explicit Readline(std::shared_ptr<App> app);
    ~Readline() = default;

    int  init();
    bool start(std::string prompt = "LwM2MClient-->> ");

    std::shared_ptr<App>& app()         { return m_app; }
    CBORAdapter&          cborAdapter() { return m_cbor; }
    CommandRegistry&      registry()    { return m_registry; }

    /// Completion callbacks need static access — these are populated
    /// by init() and read by the friend functions below.
    static CommandRegistry* s_registry;
    /// Iterators reset across rl_completion_matches invocations.
    static std::size_t s_name_idx;
    static std::size_t s_arg_idx;
    static std::string s_current_cmd;

private:
    friend char** commandCompletion(const char* text, int start, int end);
    friend char*  commandGenerator(const char* text, int state);
    friend char*  commandArgGenerator(const char* text, int state);

    static std::string trim(const std::string& s);
    /// Parse "post uri=/rd ep=foo" into ("post", {{"uri","/rd"},{"ep","foo"}}).
    /// Bare tokens (no '=') land in the map with empty value.
    static void parseLine(const std::string& line,
                          std::string& cmdName,
                          std::unordered_map<std::string, std::string>& kv);

    int executeLine(const std::string& line);

    std::shared_ptr<App>  m_app;
    CBORAdapter           m_cbor;
    CommandRegistry       m_registry;
    bool                  m_should_quit;
    std::string           m_prompt;
};

#endif /* __readline_hpp__ */
