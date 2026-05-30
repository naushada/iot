#include "cli/commands/object_alias_cmd.hpp"

#include <iostream>
#include <sstream>

#include "cli/coap_dispatch.hpp"
#include "cli/command_context.hpp"

namespace {

std::string find_or(const std::unordered_map<std::string, std::string>& kv,
                    const std::string& key, const std::string& def = "") {
    auto it = kv.find(key);
    return (it == kv.end()) ? def : it->second;
}

bool has_key(const std::unordered_map<std::string, std::string>& kv,
             const std::string& key) {
    return kv.find(key) != kv.end();
}

} // namespace

std::string ObjectAliasCmd::usage() const {
    std::ostringstream u;
    u << m_name
      << " [iid=N] [read=<rid> | write=<rid> value=<text> | "
      << "exec=<rid> [args=<text>] | observe=<rid> [cancel=true] | "
      << "delete[=<rid>]]    OID " << m_oid << " shortcut";
    return u.str();
}

Command::Result ObjectAliasCmd::execute(
        CommandContext& ctx,
        const std::unordered_map<std::string, std::string>& kv) {
    const std::string iid = find_or(kv, "iid", "0");
    const std::string oidStr = std::to_string(m_oid);

    // Build the base path /<oid>/<iid> once; per-operation branches
    // append /<rid> when the user supplied a key like read=<rid>.
    auto build_path = [&](const std::string& rid) {
        std::vector<std::string> u = {oidStr, iid};
        if (!rid.empty()) u.push_back(rid);
        return u;
    };

    // read=<rid>  → GET /<oid>/<iid>/<rid>
    if (has_key(kv, "read")) {
        return cli::dispatch(ctx, build_path(find_or(kv, "read")),
                             /*queries=*/{}, /*payload=*/{}, /*cf=*/0,
                             /*GET=*/1);
    }

    // write=<rid> value=<text>  → PUT /<oid>/<iid>/<rid> text/plain
    if (has_key(kv, "write")) {
        const std::string val = find_or(kv, "value");
        if (val.empty()) {
            std::cout << m_name << ": write= requires value=\n";
            return Command::Result::InvalidArgs;
        }
        return cli::dispatch(ctx, build_path(find_or(kv, "write")),
                             /*queries=*/{},
                             /*payload=*/{val},
                             /*cf=*/0, /*PUT=*/3);
    }

    // exec=<rid> [args=<text>]  → POST /<oid>/<iid>/<rid> text/plain
    if (has_key(kv, "exec")) {
        std::vector<std::string> payload;
        const std::string a = find_or(kv, "args");
        if (!a.empty()) payload.push_back(a);
        return cli::dispatch(ctx, build_path(find_or(kv, "exec")),
                             /*queries=*/{}, payload,
                             /*cf=*/0, /*POST=*/2);
    }

    // observe=<rid> [cancel=true]  → GET with Observe option (cancel
    // is parsed for forward-compat; see ObserveCmd note).
    if (has_key(kv, "observe")) {
        return cli::dispatch(ctx, build_path(find_or(kv, "observe")),
                             /*queries=*/{}, /*payload=*/{}, /*cf=*/0,
                             /*GET=*/1);
    }

    // delete or delete=<rid>  → DELETE /<oid>/<iid>[/<rid>]
    if (has_key(kv, "delete")) {
        return cli::dispatch(ctx, build_path(find_or(kv, "delete")),
                             /*queries=*/{}, /*payload=*/{}, /*cf=*/0,
                             /*DELETE=*/4);
    }

    // No key → bare command, read the whole instance.
    return cli::dispatch(ctx, build_path(""),
                         /*queries=*/{}, /*payload=*/{}, /*cf=*/0,
                         /*GET=*/1);
}
