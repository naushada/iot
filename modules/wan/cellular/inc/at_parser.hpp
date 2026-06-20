#ifndef __cellular_at_parser_hpp__
#define __cellular_at_parser_hpp__

#include <string>

/**
 * @file at_parser.hpp
 * @brief Pure parsers for the AT-command responses the cellular daemon needs.
 *
 * The mangOH Yellow's CF3 module (Sierra Wireless WP / Quectel) is driven over
 * an AT control channel (/dev/ttyUSB*). These free functions turn the common
 * status responses into typed values; they touch no hardware so they are fully
 * host-unit-testable. The daemon (PR-C) does the serial I/O and feeds the
 * response lines here.
 */

namespace cellular {

/// Signal quality from `+CSQ: <rssi>,<ber>`.
struct Signal {
    bool valid = false;
    int  dbm   = 0;   ///< RSSI in dBm (-113..-51); 0 when unknown
    int  bars  = 0;   ///< 0..5 derived from dbm for the UI
};

/// Operator + access technology from `+COPS: <mode>,<format>,"<oper>"[,<act>]`.
struct Operator {
    bool        valid = false;
    std::string name;          ///< e.g. "Vodafone"
    std::string tech;          ///< "2G" / "3G" / "4G" / "" (unknown)
};

/// Network registration state from `+CREG/+CGREG/+CEREG`.
enum class Reg {
    Unknown,
    NotRegistered,
    Searching,
    Denied,
    Home,
    Roaming,
};

/// `+CSQ: <rssi>,<ber>` → Signal. rssi 0..31 maps to -113..-51 dBm; 99 = unknown.
Signal parse_csq(const std::string& line);

/// `+COPS: ...` → Operator. The quoted name and optional <act> are extracted.
Operator parse_cops(const std::string& line);

/// `+CREG/+CGREG/+CEREG: [<n>,]<stat>` → Reg (uses the <stat> field).
Reg parse_creg(const std::string& line);

/// Canonical lowercase token for a Reg ("home", "roaming", "searching", …).
const char* reg_str(Reg r);

/// `+CGPADDR: <cid>,"<ip>"` (or unquoted) → the IPv4/IPv6 string, or "".
std::string parse_cgpaddr(const std::string& line);

/// ICCID from `+QCCID: <iccid>` / `+CCID: <iccid>` / a bare digit line → "".
std::string parse_iccid(const std::string& line);

} // namespace cellular

#endif /*__cellular_at_parser_hpp__*/
