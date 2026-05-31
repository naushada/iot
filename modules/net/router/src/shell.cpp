#include "shell.hpp"

#include <array>
#include <cstdio>
#include <sstream>
#include <sys/wait.h>

namespace net_router::shell {

namespace {

std::string shell_quote(const std::string& s) {
    // Wrap in single quotes; any embedded `'` becomes `'\''`. Safe
    // for /bin/sh -c.
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else           out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

} // namespace

Runner default_runner() {
    return [](const std::vector<std::string>& argv, int* exit_code) -> std::string {
        if (argv.empty()) {
            if (exit_code) *exit_code = 127;
            return {};
        }
        std::ostringstream cmd;
        for (std::size_t i = 0; i < argv.size(); ++i) {
            if (i) cmd << ' ';
            cmd << shell_quote(argv[i]);
        }
        cmd << " 2>/dev/null";

        std::FILE* fp = ::popen(cmd.str().c_str(), "r");
        if (!fp) {
            if (exit_code) *exit_code = 127;
            return {};
        }
        std::string out;
        std::array<char, 4096> buf{};
        std::size_t n = 0;
        while ((n = std::fread(buf.data(), 1, buf.size(), fp)) > 0) {
            out.append(buf.data(), n);
        }
        int status = ::pclose(fp);
        if (exit_code) {
            *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 127;
        }
        return out;
    };
}

} // namespace net_router::shell
