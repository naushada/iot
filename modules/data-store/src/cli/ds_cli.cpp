/// ds-cli — debugging client for ds-server.
///
/// Usage:
///   ds-cli [--socket=PATH] [welcome]
///   ds-cli [--socket=PATH] set <key> <value>
///   ds-cli [--socket=PATH] get <key> [<key>...]
///   ds-cli [--socket=PATH] watch [--count=N] <key> [<key>...]
///                                     # blocks reading events;
///                                     # exits on SIGINT or after
///                                     # --count=N events (default 1)
///   ds-cli [--socket=PATH] unwatch <key> [<key>...]
///
/// Exit codes: 0 ok, 1 connect/recv error, 2 protocol/arg error.

#include "data_store/client.hpp"
#include "data_store/proto.hpp"
#include "data_store/value.hpp"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>
#include <unistd.h>
#include <variant>
#include <vector>

#include "nlohmann/json.hpp"

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

void usage() {
    std::cerr <<
        "Usage:\n"
        "  ds-cli [--socket=PATH] [welcome]\n"
        "  ds-cli [--socket=PATH] set <key> <value>\n"
        "  ds-cli [--socket=PATH] get <key> [<key>...]\n"
        "  ds-cli [--socket=PATH] watch [--count=N] <key> [<key>...]\n"
        "  ds-cli [--socket=PATH] unwatch <key> [<key>...]\n";
}

struct Args {
    std::string                 socket;
    std::string                 cmd = "welcome";
    int                         count = 1;
    std::vector<std::string>    rest;
};

Args parse(int argc, char** argv) {
    Args a;
    int i = 1;

    // Leading flags.
    for (; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--socket=", 0) == 0)      a.socket = s.substr(9);
        else if (s == "--help" || s == "-h")   { usage(); std::exit(0); }
        else                                    break;
    }

    if (i < argc) { a.cmd = argv[i++]; }

    // Subcommand-scope flags + positionals.
    for (; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--count=", 0) == 0) a.count = std::atoi(s.c_str() + 8);
        else                              a.rest.emplace_back(std::move(s));
    }
    return a;
}

int do_welcome(data_store::Client& cli) {
    std::string w;
    auto rs = cli.recv_welcome(w);
    if (!rs.ok) { std::cerr << "[ds-cli] " << rs.err << "\n"; return 2; }
    std::cout << w;
    return 0;
}

/// Parse a CLI-supplied value into a Value. We try JSON first
/// (catches numbers, booleans, null, quoted strings); on parse
/// failure we fall through to a plain string. This means
/// `set foo 42` stores integer 42, `set foo true` stores bool true,
/// and `set foo hello` stores the string "hello".
data_store::Value parse_cli_value(const std::string& raw) {
    try {
        auto j = nlohmann::json::parse(raw);
        if (j.is_null())            return std::monostate{};
        if (j.is_boolean())         return j.get<bool>();
        if (j.is_string())          return j.get<std::string>();
        if (j.is_number_float())    return j.get<double>();
        if (j.is_number_unsigned()) {
            auto u = j.get<std::uint64_t>();
            if (u <= std::numeric_limits<std::uint32_t>::max())
                return static_cast<std::uint32_t>(u);
            return static_cast<double>(u);
        }
        if (j.is_number_integer()) {
            auto i = j.get<std::int64_t>();
            if (i >= std::numeric_limits<std::int32_t>::min() &&
                i <= std::numeric_limits<std::int32_t>::max())
                return static_cast<std::int32_t>(i);
            return static_cast<double>(i);
        }
    } catch (...) {
        // Not valid JSON → treat as a bare string.
    }
    return raw;
}

/// Render a Value for human-readable CLI output.
std::string value_to_display(const data_store::Value& v) {
    return std::visit([](auto&& a) -> std::string {
        using T = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<T, std::monostate>) return "(null)";
        else if constexpr (std::is_same_v<T, bool>)
            return a ? "true" : "false";
        else if constexpr (std::is_same_v<T, std::string>) return a;
        else if constexpr (std::is_same_v<T, double>) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", a);
            return buf;
        } else {
            return std::to_string(a);
        }
    }, v);
}

int do_set(data_store::Client& cli, const std::vector<std::string>& a) {
    if (a.size() != 2) { usage(); return 2; }
    std::string w; cli.recv_welcome(w);
    auto rs = cli.set(a[0], parse_cli_value(a[1]));
    if (!rs.ok) { std::cerr << "[ds-cli] set: " << rs.err << "\n"; return 2; }
    std::cout << "ok\n";
    return 0;
}

int do_get(data_store::Client& cli, const std::vector<std::string>& keys) {
    if (keys.empty()) { usage(); return 2; }
    std::string w; cli.recv_welcome(w);
    std::vector<data_store::Client::GetResult> got;
    auto rs = cli.get(keys, got);
    if (!rs.ok) { std::cerr << "[ds-cli] get: " << rs.err << "\n"; return 2; }
    for (const auto& g : got) {
        if (g.has_value) std::cout << g.key << "=" << value_to_display(g.value) << "\n";
        else             std::cout << g.key << "=(null)\n";
    }
    return 0;
}

int do_watch(data_store::Client& cli, const std::vector<std::string>& keys,
             int count) {
    if (keys.empty()) { usage(); return 2; }
    std::string w; cli.recv_welcome(w);

    // Use the callback API so the wire-level demo matches the
    // production pattern: an app registers a handler and keeps going;
    // the client's internal listener thread invokes the handler when
    // events arrive. We sleep on the main thread until `count` events
    // have fired (or SIGINT).
    std::atomic<int> received{0};
    data_store::Client::WatchHandle h = data_store::Client::kInvalidHandle;
    auto rs = cli.watch(keys,
        [&](const data_store::Client::Event& ev) {
            std::cout << "[event] " << ev.key << " = "
                      << value_to_display(ev.value);
            if (ev.prev_has_value)
                std::cout << "  (prev=" << value_to_display(ev.prev) << ")";
            std::cout << "\n";
            std::cout.flush();
            received.fetch_add(1);
        },
        &h);
    if (!rs.ok) { std::cerr << "[ds-cli] watch: " << rs.err << "\n"; return 2; }
    std::cout << "watching " << keys.size()
              << " key(s) via callback handle=" << h
              << "; count=" << count << "\n";
    std::cout.flush();

    ::signal(SIGINT,  on_signal);
    ::signal(SIGTERM, on_signal);

    while (!g_stop.load() && received.load() < count) {
        ::usleep(50 * 1000);   // 50 ms tick
    }
    cli.unwatch(h);
    return 0;
}

int do_unwatch(data_store::Client& cli, const std::vector<std::string>& keys) {
    if (keys.empty()) { usage(); return 2; }
    std::string w; cli.recv_welcome(w);
    auto rs = cli.unwatch(keys);
    if (!rs.ok) { std::cerr << "[ds-cli] unwatch: " << rs.err << "\n"; return 2; }
    std::cout << "ok\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Args a = parse(argc, argv);
    if (a.socket.empty()) a.socket = data_store::proto::kDefaultSocketPath;

    data_store::Client cli;
    auto cs = cli.connect(a.socket);
    if (!cs.ok) {
        std::cerr << "[ds-cli] connect(" << a.socket << "): " << cs.err << "\n";
        return 1;
    }

    if      (a.cmd == "welcome") return do_welcome(cli);
    else if (a.cmd == "set")     return do_set(cli, a.rest);
    else if (a.cmd == "get")     return do_get(cli, a.rest);
    else if (a.cmd == "watch")   return do_watch(cli, a.rest, a.count);
    else if (a.cmd == "unwatch") return do_unwatch(cli, a.rest);

    usage();
    return 2;
}
