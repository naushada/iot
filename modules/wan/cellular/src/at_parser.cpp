#include "at_parser.hpp"

#include <cctype>
#include <cstdlib>
#include <vector>

namespace cellular {

namespace {

/// Return the substring after the first ':' (the AT response payload), trimmed
/// of leading spaces. Returns "" if there is no ':'.
std::string after_colon(const std::string& line) {
    const auto pos = line.find(':');
    if (pos == std::string::npos) return {};
    std::size_t i = pos + 1;
    while (i < line.size() && line[i] == ' ') ++i;
    return line.substr(i);
}

/// Split on commas at top level (commas inside double quotes are kept).
std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool inq = false;
    for (char c : s) {
        if (c == '"') { inq = !inq; cur.push_back(c); }
        else if (c == ',' && !inq) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

std::string strip_quotes(std::string s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        s = s.substr(1, s.size() - 2);
    }
    return s;
}

/// Map a 3GPP <act> access-technology code to a coarse generation label.
const char* act_to_tech(int act) {
    switch (act) {
        case 0: case 1: case 3:           return "2G";   // GSM / GSM Compact / EGPRS
        case 2: case 4: case 5: case 6:   return "3G";   // UTRAN / HSDPA / HSUPA / HSPA
        case 7: case 8: case 9:           return "4G";   // E-UTRAN (LTE) families
        default:                          return "";
    }
}

} // namespace

Signal parse_csq(const std::string& line) {
    Signal s;
    const std::string body = after_colon(line);
    if (body.empty()) return s;
    const auto parts = split_csv(body);
    if (parts.empty()) return s;
    char* end = nullptr;
    const long rssi = std::strtol(parts[0].c_str(), &end, 10);
    if (end == parts[0].c_str() || rssi < 0 || rssi == 99 || rssi > 31) {
        return s;   // 99 = "not known or not detectable"
    }
    s.valid = true;
    s.dbm = static_cast<int>(-113 + 2 * rssi);
    // 5 bars across the usable RSSI window (roughly -109..-53 dBm).
    if      (s.dbm >= -65) s.bars = 5;
    else if (s.dbm >= -75) s.bars = 4;
    else if (s.dbm >= -85) s.bars = 3;
    else if (s.dbm >= -95) s.bars = 2;
    else if (s.dbm >= -105) s.bars = 1;
    else                    s.bars = 0;
    return s;
}

Operator parse_cops(const std::string& line) {
    Operator op;
    const std::string body = after_colon(line);
    if (body.empty()) return op;
    const auto parts = split_csv(body);
    // +COPS: <mode>,<format>,"<oper>"[,<act>] — name is field index 2.
    if (parts.size() >= 3) {
        op.name = strip_quotes(parts[2]);
        if (!op.name.empty()) op.valid = true;
    }
    if (parts.size() >= 4) {
        char* end = nullptr;
        const long act = std::strtol(parts[3].c_str(), &end, 10);
        if (end != parts[3].c_str()) op.tech = act_to_tech(static_cast<int>(act));
    }
    return op;
}

Reg parse_creg(const std::string& line) {
    const std::string body = after_colon(line);
    if (body.empty()) return Reg::Unknown;
    const auto parts = split_csv(body);
    // Response form is "<n>,<stat>", solicited query "<stat>". Use the last
    // purely-numeric field that looks like a <stat> (0..5).
    long stat = -1;
    for (const auto& p : parts) {
        char* end = nullptr;
        const long v = std::strtol(p.c_str(), &end, 10);
        if (end != p.c_str() && *end == '\0') stat = v;   // numeric field
        if (parts.size() == 1) break;
    }
    // When two numeric fields are present (<n>,<stat>), the loop above leaves
    // `stat` = the last one, which is <stat>. For a single field it's <stat>.
    switch (stat) {
        case 1:  return Reg::Home;
        case 2:  return Reg::Searching;
        case 3:  return Reg::Denied;
        case 5:  return Reg::Roaming;
        case 0:  return Reg::NotRegistered;
        default: return Reg::Unknown;
    }
}

const char* reg_str(Reg r) {
    switch (r) {
        case Reg::Home:          return "home";
        case Reg::Roaming:       return "roaming";
        case Reg::Searching:     return "searching";
        case Reg::Denied:        return "denied";
        case Reg::NotRegistered: return "not-registered";
        case Reg::Unknown:       default: return "unknown";
    }
}

std::string parse_cgpaddr(const std::string& line) {
    const std::string body = after_colon(line);
    if (body.empty()) return {};
    const auto parts = split_csv(body);
    // +CGPADDR: <cid>,<address>  — address is field 1, may be quoted.
    if (parts.size() >= 2) {
        std::string ip = strip_quotes(parts[1]);
        if (ip != "0.0.0.0" && !ip.empty()) return ip;
    }
    return {};
}

namespace {
    /// Lowercase copy for case-insensitive matching.
    std::string lower(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }
    bool contains(const std::string& hay, const char* needle) {
        return hay.find(needle) != std::string::npos;
    }
}

Vendor parse_vendor(const std::string& gmi_or_model) {
    const std::string s = lower(gmi_or_model);
    // Manufacturer names (AT+GMI) — unambiguous.
    if (contains(s, "sierra"))                       return Vendor::Sierra;
    if (contains(s, "quectel"))                      return Vendor::Quectel;
    if (contains(s, "u-blox") || contains(s, "ublox")) return Vendor::UBlox;
    // Model prefixes (AT+CGMM), e.g. WP7702 / BG96 / EC25 / SARA-R410.
    if (contains(s, "wp") || contains(s, "hl") || contains(s, "mc") ||
        contains(s, "em75") || contains(s, "rc7"))   return Vendor::Sierra;
    if (contains(s, "bg") || contains(s, "ec2") || contains(s, "eg2") ||
        contains(s, "ug") || contains(s, "bc"))      return Vendor::Quectel;
    if (contains(s, "sara") || contains(s, "lara") || contains(s, "lisa"))
        return Vendor::UBlox;
    return Vendor::Generic;
}

const char* iccid_command(Vendor v) {
    switch (v) {
        case Vendor::Quectel: return "AT+QCCID";
        case Vendor::Sierra:  return "AT+ICCID";
        default:              return "AT+CCID";   // u-blox + generic
    }
}

std::vector<std::string> gps_start_commands(Vendor v) {
    switch (v) {
        case Vendor::Sierra:
            // Unlock the ! commands, then start a standalone fix (max 255s,
            // 50m). NMEA then streams on the GNSS port. Re-issued periodically.
            return { "AT!ENTERCND=\"A710\"", "AT!GPSFIX=1,255,50" };
        case Vendor::Quectel:
            return { "AT+QGPS=1" };
        default:
            return {};   // u-blox/generic: assume NMEA already streaming
    }
}

std::string parse_iccid(const std::string& line) {
    std::string body = after_colon(line);
    if (body.empty()) {
        // A bare ICCID line (no "+CCID:" prefix).
        body = line;
    }
    std::string digits;
    for (char c : body) {
        if (std::isdigit(static_cast<unsigned char>(c)) || c == 'F' || c == 'f') {
            digits.push_back(c);
        }
    }
    // ICCIDs are 19–20 chars; reject obviously-too-short noise.
    return digits.size() >= 18 ? digits : std::string{};
}

} // namespace cellular
