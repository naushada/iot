#ifndef __smsctl_parser_hpp__
#define __smsctl_parser_hpp__

#include <string>
#include <vector>

#include "smsctl/command.hpp"

/**
 * @file parser.hpp
 * @brief Pure tokeniser + parser for the `IOT …` SMS command grammar.
 *
 * Pure and host-testable: no ds, no modem, no clock. See the grammar table in
 * apps/docs/tdd-smsctl.md.
 */

namespace smsctl {

/// Split on whitespace, honouring double-quoted groups (`"my ssid"`) and
/// backslash-escaped quotes (`\"`). An unterminated quote consumes the rest
/// of the line (forgiving — an operator's phone may auto-"smarten" quotes).
std::vector<std::string> tokenize(const std::string& text);

/// Parse an inbound SMS body. Text without the `IOT ` prefix (case-
/// insensitive) yields `Kind::NotACommand` — the caller drops it in silence,
/// so ordinary messages and carrier spam never trigger a reply.
Command parse(const std::string& text);

} // namespace smsctl

#endif /* __smsctl_parser_hpp__ */
