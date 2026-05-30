#include "readline.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <sstream>

#include "cli/command.hpp"
#include "cli/command_context.hpp"

extern "C" {
    #include <stdio.h>
    #include <string.h>
    #include <readline/readline.h>
    #include <readline/history.h>
}

// Forward decls so init() can install the completion hook. The
// friend declarations inside class Readline do not introduce these
// names at namespace scope.
char** commandCompletion(const char* text, int start, int end);
char*  commandGenerator(const char* text, int state);
char*  commandArgGenerator(const char* text, int state);

CommandRegistry* Readline::s_registry    = nullptr;
std::size_t      Readline::s_name_idx    = 0;
std::size_t      Readline::s_arg_idx     = 0;
std::string      Readline::s_current_cmd;

Readline::Readline(std::shared_ptr<App> app)
  : m_app(std::move(app)),
    m_registry(CommandRegistry::build_default()),
    m_should_quit(false) {
}

int Readline::init() {
    rl_attempted_completion_over  = 1;
    rl_attempted_completion_function = commandCompletion;
    s_registry = &m_registry;
    return 0;
}

std::string Readline::trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t");
    if (first == std::string::npos) return "";
    const auto last = s.find_last_not_of(" \t");
    return s.substr(first, last - first + 1);
}

void Readline::parseLine(const std::string& line,
                         std::string& cmdName,
                         std::unordered_map<std::string, std::string>& kv) {
    std::istringstream iss(line);
    iss >> cmdName;
    std::string token;
    while (iss >> token) {
        const auto eq = token.find('=');
        if (eq == std::string::npos) {
            kv[token] = "";
        } else {
            kv[token.substr(0, eq)] = token.substr(eq + 1);
        }
    }
}

int Readline::executeLine(const std::string& line) {
    std::string cmdName;
    std::unordered_map<std::string, std::string> kv;
    parseLine(line, cmdName, kv);
    if (cmdName.empty()) return 0;

    Command* cmd = m_registry.find(cmdName);
    if (!cmd) {
        std::cout << "unknown command: " << cmdName << " (try 'help')\n";
        return -1;
    }

    CommandContext ctx{m_app, m_cbor, m_should_quit, m_registry};
    auto rc = cmd->execute(ctx, kv);
    if (rc == Command::Result::InvalidArgs) {
        std::cout << "usage: " << cmd->usage() << "\n";
    }
    return 0;
}

bool Readline::start(std::string PS) {
    m_prompt = std::move(PS);
    while (!m_should_quit) {
        std::unique_ptr<char, void(*)(void*)> raw(readline(m_prompt.c_str()), &std::free);
        if (!raw) break;
        std::string line = trim(std::string(raw.get()));
        if (line.empty()) continue;
        add_history(line.c_str());
        executeLine(line);
    }
    return true;
}

// --- tab-completion --------------------------------------------------
//
// GNU readline calls these as plain C function pointers. They reach
// the registry via Readline::s_registry, set in init().

char* commandGenerator(const char* text, int state) {
    if (!Readline::s_registry) return nullptr;
    if (!state) Readline::s_name_idx = 0;
    const std::string prefix(text);
    const auto& names = Readline::s_registry->names();
    while (Readline::s_name_idx < names.size()) {
        const std::string& n = names[Readline::s_name_idx++];
        if (n.compare(0, prefix.size(), prefix) == 0) {
            return strdup(n.c_str());
        }
    }
    return nullptr;
}

char* commandArgGenerator(const char* text, int state) {
    if (!Readline::s_registry) return nullptr;
    if (!state) Readline::s_arg_idx = 0;
    Command* cmd = Readline::s_registry->find(Readline::s_current_cmd);
    if (!cmd) return nullptr;
    const auto args = cmd->args();
    const std::string prefix(text);
    while (Readline::s_arg_idx < args.size()) {
        const std::string& a = args[Readline::s_arg_idx++];
        if (a.compare(0, prefix.size(), prefix) == 0) {
            return strdup(a.c_str());
        }
    }
    return nullptr;
}

char** commandCompletion(const char* text, int start, int /*end*/) {
    if (start == 0) {
        return rl_completion_matches(text, commandGenerator);
    }
    // Argument-position completion. Extract the command name typed so
    // far so commandArgGenerator can look it up.
    std::string line(rl_line_buffer);
    auto sp = line.find(' ');
    Readline::s_current_cmd = (sp == std::string::npos) ? line : line.substr(0, sp);
    return rl_completion_matches(text, commandArgGenerator);
}
