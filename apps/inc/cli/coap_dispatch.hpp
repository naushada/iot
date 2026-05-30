#ifndef __cli_coap_dispatch_hpp__
#define __cli_coap_dispatch_hpp__

#include <cstdint>
#include <string>
#include <vector>

#include "cli/command.hpp"

class CommandContext;

namespace cli {

/// Build and ship one or more CoAP request frames to the
/// LwM2MClient ServiceContext. Returns Command::Result::Ok when at
/// least one frame was successfully enqueued, DispatchFailed
/// otherwise.
///
/// `uris` is the path split by '/' (e.g. {"rd", "0"} → /rd/0).
/// `queries` is the query split by '&' (e.g. {"ep=foo", "lt=60"}).
/// `payload` is zero or more already-encoded request bodies (the
/// existing CoAPAdapter::serialise contract takes a vector; pass {}
/// for no body).
/// `cf` is the Content-Format option value (0 = text/plain, 60 = CBOR,
/// 11542 = OMA TLV, …). Ignored when payload is empty.
/// `method` follows the CoAP method codes: 1=GET 2=POST 3=PUT 4=DELETE.
Command::Result dispatch(CommandContext& ctx,
                         const std::vector<std::string>& uris,
                         const std::vector<std::string>& queries,
                         const std::vector<std::string>& payload,
                         std::uint16_t                   cf,
                         std::uint8_t                    method);

/// Split `in` on `delim`, preserving quoted segments. Lifted from the
/// old Readline::str2Vector so every command can parse uri= and
/// uri-query= identically.
std::vector<std::string> split(const std::string& in, char delim);

} // namespace cli

#endif /* __cli_coap_dispatch_hpp__ */
