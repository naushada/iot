#include "cli/commands/post_cmd.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>

#include "cbor_adapter.hpp"
#include "cli/coap_dispatch.hpp"
#include "cli/command_context.hpp"
#include "coap_adapter.hpp"

namespace {

std::uint8_t parse_method(const std::string& s) {
    std::string m = s;
    std::transform(m.begin(), m.end(), m.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    if (m == "GET")    return 1;
    if (m == "POST")   return 2;
    if (m == "PUT")    return 3;
    if (m == "DELETE") return 4;
    return 2; // default: POST
}

std::string find_or(const std::unordered_map<std::string, std::string>& kv,
                    const std::string& key, const std::string& fallback = "") {
    auto it = kv.find(key);
    return (it == kv.end()) ? fallback : it->second;
}

} // namespace

Command::Result PostCmd::execute(CommandContext& ctx,
                                  const std::unordered_map<std::string, std::string>& kv) {
    const std::string uri    = find_or(kv, "uri");
    const std::string query  = find_or(kv, "uri-query");
    const std::string method = find_or(kv, "method", "POST");
    const std::string data   = find_or(kv, "data");
    const std::string file   = find_or(kv, "file");
    const std::string cfStr  = find_or(kv, "content-format", "0");

    if (uri.empty()) {
        std::cout << "post: missing uri=\n";
        return Command::Result::InvalidArgs;
    }

    std::vector<std::string> uris    = cli::split(uri, '/');
    std::vector<std::string> queries = cli::split(query, '&');
    std::uint16_t cf = 0;
    try { cf = static_cast<std::uint16_t>(std::stoi(cfStr)); }
    catch (...) { cf = 0; }

    // Payload precedence: file= > data=. Matches the legacy behaviour
    // documented in apps/src/readline.cpp BUG-002 fix.
    std::vector<std::string> payload;
    if (!file.empty()) {
        CoAPAdapter coap;
        std::string content = ctx.cbor.getJson(file);
        coap.buildRequest(content, payload);
    } else if (!data.empty() && data.length() <= 1024) {
        std::string encoded;
        ctx.cbor.json2cbor(data, encoded);
        if (!encoded.empty()) payload.push_back(std::move(encoded));
    }

    return cli::dispatch(ctx, uris, queries, payload, cf, parse_method(method));
}
