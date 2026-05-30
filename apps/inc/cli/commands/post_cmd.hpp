#ifndef __cli_commands_post_cmd_hpp__
#define __cli_commands_post_cmd_hpp__

#include "cli/command.hpp"

/// Low-level CoAP send. `post` is the escape hatch behind every
/// LwM2M-level command — useful for hand-crafted requests against
/// /push, /set, /.get, /.execute and similar non-standard URIs.
///
/// Args:
///   uri=/x/y           required, path split on '/'
///   method=POST|GET|PUT|DELETE   defaults to POST
///   uri-query=k=v&…    optional
///   data=<json>        optional, json2cbor'd
///   file=<path>        optional, raw bytes read from disk; takes
///                      precedence over data=
///   content-format=N   optional, default 0
class PostCmd : public Command {
public:
    std::string name() const override { return "post"; }
    std::string usage() const override {
        return "post uri=/p/q [method=POST|GET|PUT|DELETE] "
               "[uri-query=k=v&…] [data=<json>] [file=<path>] "
               "[content-format=N]";
    }
    std::vector<std::string> args() const override {
        return {"uri=", "method=", "uri-query=", "data=", "file=", "content-format="};
    }
    Result execute(CommandContext& ctx,
                   const std::unordered_map<std::string, std::string>& kv) override;
};

#endif /* __cli_commands_post_cmd_hpp__ */
