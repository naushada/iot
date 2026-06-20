#include "nmea_parser.hpp"

#include <cstdlib>
#include <vector>

namespace cellular {

namespace {

/// Split a sentence body on commas (NMEA has no quoting). Trailing checksum is
/// stripped by the caller before splitting.
std::vector<std::string> split_fields(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

double to_double(const std::string& s) {
    if (s.empty()) return 0.0;
    return std::strtod(s.c_str(), nullptr);
}

/// NMEA latitude/longitude come as ddmm.mmmm / dddmm.mmmm. Convert to signed
/// decimal degrees using the hemisphere field (N/E +, S/W -).
double dm_to_deg(const std::string& dm, const std::string& hemi, int deg_digits) {
    if (dm.size() < static_cast<std::size_t>(deg_digits)) return 0.0;
    const double whole = to_double(dm.substr(0, deg_digits));
    const double minutes = to_double(dm.substr(deg_digits));
    double deg = whole + minutes / 60.0;
    if (hemi == "S" || hemi == "W") deg = -deg;
    return deg;
}

/// Return the sentence type token (e.g. "GGA") from "$GPGGA" / "$GNGGA".
std::string talker_type(const std::string& sentence) {
    // "$ttSSS,..." → the 3 chars after the 2-char talker id.
    if (sentence.size() < 6 || sentence[0] != '$') return {};
    return sentence.substr(3, 3);
}

} // namespace

bool nmea_checksum_ok(const std::string& sentence) {
    const auto star = sentence.find('*');
    if (sentence.empty() || sentence[0] != '$' || star == std::string::npos) {
        return false;
    }
    if (star + 3 > sentence.size()) return false;   // need 2 hex chars
    unsigned char sum = 0;
    for (std::size_t i = 1; i < star; ++i) {
        sum ^= static_cast<unsigned char>(sentence[i]);
    }
    const long given = std::strtol(sentence.substr(star + 1, 2).c_str(), nullptr, 16);
    return static_cast<long>(sum) == given;
}

namespace {
/// Common front-matter for both parsers: validate checksum + type, return the
/// comma-split fields (body between '$' and '*').
bool fields_for(const std::string& sentence, const char* want,
                std::vector<std::string>& fields) {
    if (!nmea_checksum_ok(sentence)) return false;
    if (talker_type(sentence) != want) return false;
    const auto star = sentence.find('*');
    fields = split_fields(sentence.substr(1, star - 1));   // drop '$' and '*..'
    return true;
}
} // namespace

bool parse_gga(const std::string& sentence, GpsFix& out) {
    std::vector<std::string> f;
    if (!fields_for(sentence, "GGA", f)) return false;
    // 0:type 1:utc 2:lat 3:N/S 4:lon 5:E/W 6:qual 7:sats 8:hdop 9:alt 10:M ...
    if (f.size() < 10) return false;
    const int qual = static_cast<int>(std::strtol(f[6].c_str(), nullptr, 10));
    out.utc  = f[1];
    out.sats = static_cast<int>(std::strtol(f[7].c_str(), nullptr, 10));
    out.alt_m = to_double(f[9]);
    if (qual > 0 && !f[2].empty() && !f[4].empty()) {
        out.lat = dm_to_deg(f[2], f[3], 2);
        out.lon = dm_to_deg(f[4], f[5], 3);
        out.valid = true;
        out.quality = "3d";          // GGA with a fix; RMC refines 2d vs 3d if needed
    } else {
        out.quality = "none";
    }
    return true;
}

bool parse_rmc(const std::string& sentence, GpsFix& out) {
    std::vector<std::string> f;
    if (!fields_for(sentence, "RMC", f)) return false;
    // 0:type 1:utc 2:status 3:lat 4:N/S 5:lon 6:E/W 7:speed(kn) 8:course 9:date
    if (f.size() < 9) return false;
    out.utc = f[1];
    const bool active = (f[2] == "A");
    if (active && !f[3].empty() && !f[5].empty()) {
        out.lat = dm_to_deg(f[3], f[4], 2);
        out.lon = dm_to_deg(f[5], f[6], 3);
        out.speed_kmh = to_double(f[7]) * 1.852;   // knots → km/h
        out.course_deg = to_double(f[8]);
        out.valid = true;
        if (out.quality.empty() || out.quality == "none") out.quality = "2d";
    } else if (out.quality.empty()) {
        out.quality = "none";
    }
    return true;
}

} // namespace cellular
