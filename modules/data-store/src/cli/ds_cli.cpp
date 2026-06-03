/// ds-cli — debugging client for ds-server (EMP protocol).
///
/// Usage:
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
#include <set>
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
        "  ds-cli [--socket=PATH] set <key> <value>\n"
        "  ds-cli [--socket=PATH] get <key> [<key>...]\n"
        "  ds-cli [--socket=PATH] watch [--count=N] <key> [<key>...]\n"
        "  ds-cli [--socket=PATH] unwatch <key> [<key>...]\n"
        "  ds-cli [--socket=PATH] svc list\n"
        "  ds-cli [--socket=PATH] svc enable  <name>\n"
        "  ds-cli [--socket=PATH] svc disable <name> [--until-boot]\n"
        "  ds-cli [--socket=PATH] svc status  <name>\n";
}

struct Args {
    std::string                 socket;
    std::string                 cmd;
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
    auto rs = cli.set(a[0], parse_cli_value(a[1]));
    if (!rs.ok) { std::cerr << "[ds-cli] set: " << rs.err << "\n"; return 2; }
    std::cout << "ok\n";
    return 0;
}

int do_get(data_store::Client& cli, const std::vector<std::string>& keys) {
    if (keys.empty()) { usage(); return 2; }
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
    auto rs = cli.unwatch(keys);
    if (!rs.ok) { std::cerr << "[ds-cli] unwatch: " << rs.err << "\n"; return 2; }
    std::cout << "ok\n";
    return 0;
}

} // namespace

int do_svc(data_store::Client& cli, const std::vector<std::string>& rest);

int main(int argc, char** argv) {
    Args a = parse(argc, argv);
    if (a.socket.empty()) a.socket = data_store::proto::kDefaultSocketPath;

    data_store::Client cli;
    auto cs = cli.connect(a.socket);
    if (!cs.ok) {
        std::cerr << "[ds-cli] connect(" << a.socket << "): " << cs.err << "\n";
        return 1;
    }

    if      (a.cmd == "set")     return do_set(cli, a.rest);
    else if (a.cmd == "get")     return do_get(cli, a.rest);
    else if (a.cmd == "watch")   return do_watch(cli, a.rest, a.count);
    else if (a.cmd == "unwatch") return do_unwatch(cli, a.rest);
    else if (a.cmd == "svc")     return do_svc(cli, a.rest);

    usage();
    return 2;
}

namespace {

/// Render one services.* row to stdout. Looks up enable + state
/// + (optionally) uptime.sec per name.
void render_svc_row(data_store::Client& cli, const std::string& name,
                    const std::string& deps) {
    const std::string en_k = "services." + name + ".enable";
    const std::string st_k = "services." + name + ".state";
    const std::string up_k = "services." + name + ".uptime.sec";
    std::vector<data_store::Client::GetResult> got;
    cli.get({en_k, st_k, up_k}, got);
    auto find = [&](const std::string& k) -> std::string {
        for (const auto& g : got) {
            if (g.key == k && g.has_value) {
                if (auto s = data_store::to_string(g.value)) return *s;
                if (auto n = data_store::to_int32(g.value))
                    return std::to_string(*n);
                if (auto u = data_store::to_uint32(g.value))
                    return std::to_string(*u);
                if (auto b = data_store::to_bool(g.value))
                    return *b ? "true" : "false";
                return "?";
            }
        }
        return "-";
    };
    std::string enable = (name == "ds") ? std::string("n/a") : find(en_k);
    std::string state  = find(st_k);
    std::string uptime = find(up_k);
    std::string dep_col = deps.empty() ? "-" : deps;
    std::printf("%-18s %-8s %-10s %-12s %s\n",
                name.c_str(), enable.c_str(), state.c_str(),
                dep_col.c_str(),
                uptime == "-" ? "" : uptime.c_str());
}

} // namespace

int do_svc(data_store::Client& cli, const std::vector<std::string>& rest) {
    if (rest.empty()) {
        usage();
        return 2;
    }
    const std::string verb = rest[0];

    if (verb == "list") {
        std::string body;
        auto rs = cli.schema_dump(body);
        if (!rs.ok) {
            std::cerr << "[ds-cli] svc list: schema-dump failed: "
                      << rs.err << "\n";
            return 1;
        }
        // Parse the schema JSON and extract every distinct
        // "services.<name>." prefix.
        nlohmann::json doc;
        try { doc = nlohmann::json::parse(body); }
        catch (const std::exception& e) {
            std::cerr << "[ds-cli] svc list: bad schema JSON: "
                      << e.what() << "\n";
            return 1;
        }
        std::vector<std::string> names;
        // L17a/D5 — map service name → comma-joined depends_on list
        // for the DEPENDS column.
        std::map<std::string, std::string> deps_map;
        if (doc.contains("keys") && doc["keys"].is_object()) {
            std::set<std::string> seen;
            for (auto it = doc["keys"].begin(); it != doc["keys"].end(); ++it) {
                const std::string& key = it.key();
                if (key.rfind("services.", 0) != 0) continue;
                auto rest_str = key.substr(9);
                // Drop the trailing ".enable" / ".state" / ".uptime.sec"
                // by trimming at the LAST dot. "services.ds.uptime.sec"
                // -> "ds.uptime" — that's wrong; we want "ds". Better:
                // walk until we hit ".enable" or ".state" or
                // ".uptime.sec" suffix.
                static const char* suffixes[] = {
                    ".enable", ".state", ".uptime.sec",
                };
                for (auto suf : suffixes) {
                    auto slen = std::strlen(suf);
                    if (rest_str.size() >= slen &&
                        rest_str.compare(rest_str.size() - slen, slen, suf) == 0) {
                        rest_str.resize(rest_str.size() - slen);
                        break;
                    }
                }
                if (!rest_str.empty()) {
                    seen.insert(rest_str);
                    // L17a/D5 — extract depends_on if present
                    if (it.value().contains("depends_on") &&
                        it.value()["depends_on"].is_array()) {
                        std::string joined;
                        for (const auto& dep : it.value()["depends_on"]) {
                            if (dep.is_string()) {
                                if (!joined.empty()) joined += ',';
                                joined += dep.get<std::string>();
                            }
                        }
                        deps_map[rest_str] = joined;
                    }
                }
            }
            names.assign(seen.begin(), seen.end());
        }
        std::printf("%-18s %-8s %-10s %-12s %s\n",
                    "NAME", "ENABLE", "STATE", "DEPENDS", "UPTIME");
        for (const auto& n : names) {
            auto dit = deps_map.find(n);
            render_svc_row(cli, n, dit != deps_map.end() ? dit->second : "");
        }
        return 0;
    }

    if (verb == "enable" || verb == "disable") {
        // L17b — support --until-boot for volatile disable.
        // enable ignores --until-boot (no-op; enable is always persistent).
        bool until_boot = false;
        std::string name;
        for (std::size_t i = 1; i < rest.size(); ++i) {
            if (rest[i] == "--until-boot") {
                until_boot = true;
            } else {
                name = rest[i];
            }
        }
        if (name.empty()) { usage(); return 2; }
        if (name == "ds") {
            std::cerr << "[ds-cli] svc " << verb
                      << ": ds is substrate; cannot be "
                      << verb << "d via the data store (use "
                      << "systemctl)\n";
            return 1;
        }
        const std::string key = "services." + name + ".enable";
        const bool val = (verb == "enable");
        auto rs = (until_boot && verb == "disable")
                ? cli.set_volatile(key, val)
                : cli.set(key, val);
        if (!rs.ok) {
            std::cerr << "[ds-cli] svc " << verb << ": " << rs.err << "\n";
            return 2;
        }
        if (until_boot) {
            std::cout << "ok  (volatile — survives until ds-server restart)\n";
        } else {
            std::cout << "ok\n";
        }
        return 0;
    }

    if (verb == "status") {
        if (rest.size() < 2) { usage(); return 2; }
        const std::string& name = rest[1];
        std::vector<std::string> keys = {
            "services." + name + ".enable",
            "services." + name + ".state",
        };
        if (name == "ds") {
            keys[0] = "services.ds.state";   // ds has no .enable
            keys.push_back("services.ds.uptime.sec");
        }
        std::vector<data_store::Client::GetResult> got;
        auto rs = cli.get(keys, got);
        if (!rs.ok) {
            std::cerr << "[ds-cli] svc status: " << rs.err << "\n";
            return 1;
        }
        for (const auto& g : got) {
            std::cout << g.key << " = ";
            if (!g.has_value) { std::cout << "(unset)\n"; continue; }
            if (auto s = data_store::to_string(g.value)) std::cout << *s;
            else if (auto b = data_store::to_bool(g.value))
                std::cout << (*b ? "true" : "false");
            else if (auto n = data_store::to_int32(g.value))
                std::cout << *n;
            else if (auto u = data_store::to_uint32(g.value))
                std::cout << *u;
            else std::cout << "(?)";
            std::cout << "\n";
        }
        return 0;
    }

    usage();
    return 2;
}
