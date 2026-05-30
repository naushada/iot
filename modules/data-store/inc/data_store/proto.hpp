#ifndef __data_store_proto_hpp__
#define __data_store_proto_hpp__

/// Shared by client + server. Wire-protocol constants and string
/// helpers per modules/data-store/docs/design.md §3.
///
/// One JSON document per `\n`-terminated line, both directions.
/// Helpers in this header avoid pulling nlohmann::json into every
/// translation unit — see proto.cpp for the parsers.

#include <string>
#include <vector>

namespace data_store::proto {

/// All supported request operations.
enum class Op {
    Set,
    Get,
    Register,
    Remove,
    Unknown,
};

/// Parse op-name strings the server is willing to accept. Unknown
/// strings map to Op::Unknown so the dispatcher can reject them
/// uniformly (REQ-DS-014).
Op parse_op(const std::string& s);
std::string op_name(Op op);

/// Welcome line sent by the server on connect. Single line, ends
/// with '\n'. D1 risk-gate uses this exact string.
constexpr const char* kWelcomeLine =
    "{\"ok\":true,\"hello\":\"data-store-server\",\"v\":1}\n";

/// Default unix-socket path and persistence file location, both
/// overridable via CLI flags on the server.
constexpr const char* kDefaultSocketPath = "/var/run/iot/data_store.sock";
constexpr const char* kDefaultStorePath  = "/var/lib/iot/data_store.lua";

} // namespace data_store::proto

#endif /* __data_store_proto_hpp__ */
