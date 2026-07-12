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

/// Drop leading/trailing spaces and tabs. split_csv() keeps whatever padding a
/// modem puts around its fields; +CGCONTRDP is one that does.
std::string trim_spaces(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t')) --b;
    return s.substr(a, b - a);
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

Reg best_reg(Reg cs, Reg ps, Reg eps) {
    auto rank = [](Reg r) {
        switch (r) {
            case Reg::Home:          return 5;
            case Reg::Roaming:       return 4;
            case Reg::Searching:     return 3;
            case Reg::Denied:        return 2;
            case Reg::NotRegistered: return 1;
            case Reg::Unknown:       default: return 0;
        }
    };
    Reg best = cs;
    if (rank(ps)  > rank(best)) best = ps;
    if (rank(eps) > rank(best)) best = eps;
    return best;
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
    /// Strict dotted-quad check. Rejects the unspecified address, and rejects the
    /// 8-group form 3GPP uses for "<local_addr> and <subnet_mask>" as well as the
    /// 16-group dotted form it uses for IPv6 — we only publish IPv4 resolvers.
    bool is_ipv4_dotted_quad(const std::string& s) {
        if (s.empty() || s == "0.0.0.0") return false;
        int groups = 0;
        std::size_t i = 0;
        while (i <= s.size()) {
            std::size_t dot = s.find('.', i);
            const std::string g = s.substr(i, dot == std::string::npos ? std::string::npos : dot - i);
            if (g.empty() || g.size() > 3) return false;
            for (char c : g) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
            if (std::atoi(g.c_str()) > 255) return false;
            ++groups;
            if (dot == std::string::npos) break;
            i = dot + 1;
        }
        return groups == 4;
    }
}

std::string parse_cgcontrdp_dns(const std::string& line) {
    const std::string body = after_colon(line);
    if (body.empty()) return {};
    // 3GPP 27.007: +CGCONTRDP: <cid>,<bearer_id>,<apn>,<local_addr and subnet_mask>
    //              ,<gw_addr>,<DNS_prim>,<DNS_sec>[,...]
    // The secondary resolver is optional, and either may be absent/empty.
    const auto parts = split_csv(body);
    std::string out;
    for (std::size_t i = 5; i <= 6 && i < parts.size(); ++i) {
        const std::string dns = strip_quotes(trim_spaces(parts[i]));
        if (!is_ipv4_dotted_quad(dns)) continue;
        if (!out.empty()) out.push_back(',');
        out += dns;
    }
    return out;
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

int selrat_index(const std::string& rat) {
    const std::string s = lower(rat);
    if (s == "auto" || s == "automatic" || s == "all")   return 0;
    if (s == "umts" || s == "3g")                        return 1;
    if (s == "gsm"  || s == "2g")                        return 2;
    if (s == "gsm+umts")                                 return 5;
    if (s == "lte"  || s == "4g")                        return 6;
    if (s == "gsm+umts+lte")                             return 7;
    if (s == "umts+lte")                                 return 11;
    if (s == "gsm+lte")                                  return 12;
    return -1;
}

std::string parse_selrat(const std::string& line) {
    if (!contains(lower(line), "!selrat:")) return {};
    // "!SELRAT: 06, LTE Only" → the name after the comma; else the whole tail.
    const std::string body = after_colon(line);
    const auto comma = body.find(',');
    std::string name = (comma == std::string::npos) ? body : body.substr(comma + 1);
    std::size_t a = 0, b = name.size();
    while (a < b && (name[a] == ' ' || name[a] == '\t')) ++a;
    while (b > a && (name[b-1] == ' ' || name[b-1] == '\t' || name[b-1] == '\r')) --b;
    return name.substr(a, b - a);
}

std::string parse_ceer(const std::string& line) {
    if (!contains(lower(line), "+ceer:")) return {};
    std::string body = after_colon(line);
    std::size_t a = 0, b = body.size();
    while (a < b && body[a] == ' ') ++a;
    while (b > a && (body[b-1] == ' ' || body[b-1] == '\r')) --b;
    return body.substr(a, b - a);
}

std::string parse_cnum(const std::string& line) {
    if (!contains(lower(line), "+cnum:")) return {};
    const auto parts = split_csv(after_colon(line));
    // +CNUM: <alpha>,<number>,<type> — the number is field 1.
    if (parts.size() >= 2) return strip_quotes(parts[1]);
    return {};
}

std::string parse_imei(const std::string& line) {
    if (lower(line).rfind("imei:", 0) != 0) return {};   // "IMEI:" only (not "IMEI SV:")
    std::string d;
    for (char c : after_colon(line)) if (c >= '0' && c <= '9') d.push_back(c);
    return d.size() >= 14 ? d : std::string{};
}

std::string parse_labeled(const std::string& line, const char* label) {
    const std::string lbl = lower(std::string(label)) + ":";
    if (lower(line).rfind(lbl, 0) != 0) return {};
    std::string v = after_colon(line);
    std::size_t a = 0, b = v.size();
    while (a < b && (v[a] == ' ' || v[a] == '\t')) ++a;
    while (b > a && (v[b-1] == ' ' || v[b-1] == '\t' || v[b-1] == '\r')) --b;
    return v.substr(a, b - a);
}

std::string model_capability(const std::string& model) {
    const std::string m = lower(model);
    if (contains(m, "wp77") || contains(m, "hl78") || contains(m, "bg96"))
        return "LTE-M / NB-IoT / GSM";
    if (contains(m, "wp76") || contains(m, "em75") || contains(m, "ec25"))
        return "LTE Cat-4 / 3G / 2G";
    return {};
}

std::string parse_cgdcont(const std::string& line) {
    if (!contains(lower(line), "+cgdcont:")) return {};
    const auto parts = split_csv(after_colon(line));
    // +CGDCONT: <cid>,"<PDP_type>","<apn>",... — the APN is field 2.
    if (parts.size() < 3) return {};
    // ONLY cid 1. `AT+CGDCONT?` answers with one line per provisioned context,
    // and the caller feeds us every one of them — so without this the LAST cid
    // listed won, not the one we use. A WP7702 carries Sierra's own profiles
    // (iot.swir on a higher cid), so cell.apn.current reported iot.swir while
    // the data call was actually on the operator APN we wrote to cid 1. cid 1 is
    // the context this stack provisions (AT+CGDCONT=1) and reads back
    // (AT+CGCONTRDP=1); the other cids are none of our business.
    if (strip_quotes(parts[0]) != "1") return {};
    return strip_quotes(parts[2]);
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

std::string parse_cpms(const std::string& line) {
    if (!contains(lower(line), "+cpms:")) return {};
    // `+CPMS: "SM",2,30,"SM",2,30,"SM",2,30` — the first <used>,<total>
    // pair (fields 1 and 2) describes the receive/read store.
    const std::string body = after_colon(line);
    std::vector<std::string> fields;
    std::string cur;
    for (char c : body) {
        if (c == ',') { fields.push_back(cur); cur.clear(); }
        else          { cur.push_back(c); }
    }
    fields.push_back(cur);
    if (fields.size() < 3) return {};
    auto num = [](const std::string& f) -> std::string {
        std::string d;
        for (char c : f)
            if (std::isdigit(static_cast<unsigned char>(c))) d.push_back(c);
            else if (c != ' ' && c != '\t' && c != '\r') return std::string{};
        return d;
    };
    const std::string used  = num(fields[1]);
    const std::string total = num(fields[2]);
    if (used.empty() || total.empty()) return {};
    return used + "/" + total;
}

} // namespace cellular
