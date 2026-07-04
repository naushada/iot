#ifndef __cellular_at_parser_hpp__
#define __cellular_at_parser_hpp__

#include <string>
#include <vector>

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

/// Combine the CS (`+CREG`), PS (`+CGREG`) and EPS (`+CEREG`) registration
/// states into the best one (registered > searching > denied > not-registered >
/// unknown; Home preferred over Roaming). A modem camped on 2G registers via
/// `+CREG` while `+CEREG` (LTE) reads not-registered — polling only CEREG
/// misreports it as offline, so the daemon polls all three and combines here.
Reg best_reg(Reg cs, Reg ps, Reg eps);

/// The Sierra `AT!SELRAT=<n>` index for a friendly RAT token, or -1 if unknown:
///   "auto"/"all" → 0, "umts"/"3g" → 1, "gsm"/"2g" → 2, "gsm+umts" → 5,
///   "lte"/"4g" → 6, "gsm+umts+lte" → 7, "umts+lte" → 11, "gsm+lte" → 12.
int selrat_index(const std::string& rat);

/// Human RAT name from a `!SELRAT: <n>, <name>` reply → "<name>" (e.g.
/// "LTE Only"), or "" if not a SELRAT line.
std::string parse_selrat(const std::string& line);

/// Failure/reject cause text from a `+CEER: <text>` reply → "<text>", or "".
/// (Some firmwares answer `AT+CEER` with ERROR — the daemon treats that as no
/// reason, i.e. this returns "" for a non-`+CEER:` line.)
std::string parse_ceer(const std::string& line);

/// Subscriber number from `+CNUM: <alpha>,"<number>",<type>` → "<number>", or ""
/// (this record is often empty on IoT SIMs).
std::string parse_cnum(const std::string& line);

/// IMEI from an `ATI` "IMEI: <digits>" line (excludes the "IMEI SV:" line) → the
/// digit string (>=14), or "".
std::string parse_imei(const std::string& line);

/// The trimmed value of an `ATI` "<Label>: <value>" line (case-insensitive),
/// e.g. parse_labeled(line, "Model") on "Model: WP7702" → "WP7702"; "" if the
/// line does not start with that label.
std::string parse_labeled(const std::string& line, const char* label);

/// Coarse RAT capability string for a modem model (e.g. WP7702 →
/// "LTE-M / NB-IoT / GSM"), or "" if unknown.
std::string model_capability(const std::string& model);

/// APN from a `+CGDCONT: <cid>,"IP","<apn>",...` context line → "<apn>", or ""
/// (an undefined/empty context). Used to read back the provisioned data APN.
std::string parse_cgdcont(const std::string& line);

/// `+CGPADDR: <cid>,"<ip>"` (or unquoted) → the IPv4/IPv6 string, or "".
std::string parse_cgpaddr(const std::string& line);

/// ICCID from `+QCCID:` / `+CCID:` / `+ICCID:` / a bare digit line → "".
std::string parse_iccid(const std::string& line);

/// Modem firmware family — the AT command set differs per vendor. Detected
/// from `AT+GMI` (manufacturer) or `AT+CGMM` (model).
enum class Vendor {
    Generic,   ///< unknown — use the most standard commands
    Sierra,    ///< Sierra Wireless AirPrime (WP/HL/MC/EM/RC…) — AT!GPS*, AT+ICCID
    Quectel,   ///< Quectel (BG/EC/EG/UG…) — AT+QGPS*, AT+QCCID
    UBlox,     ///< u-blox (SARA/LARA/LISA)
};

/// Classify a vendor from an `AT+GMI` / `AT+CGMM` reply line (case-insensitive;
/// matches manufacturer names and known model prefixes). Generic if unknown.
Vendor parse_vendor(const std::string& gmi_or_model);

/// The ICCID query command for this vendor (Quectel `AT+QCCID`, Sierra
/// `AT+ICCID`, else the standard `AT+CCID`).
const char* iccid_command(Vendor v);

/// The GNSS start command sequence for this vendor (issued before reading a
/// fix). Sierra unlocks then starts a standalone fix (`AT!ENTERCND` +
/// `AT!GPSFIX`); Quectel powers the engine (`AT+QGPS=1`); empty for Generic.
std::vector<std::string> gps_start_commands(Vendor v);

} // namespace cellular

#endif /*__cellular_at_parser_hpp__*/
