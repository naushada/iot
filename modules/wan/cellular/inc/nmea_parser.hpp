#ifndef __cellular_nmea_parser_hpp__
#define __cellular_nmea_parser_hpp__

#include <string>

/**
 * @file nmea_parser.hpp
 * @brief Pure NMEA-0183 GNSS sentence parsers (GGA + RMC).
 *
 * The WP module's GNSS emits NMEA on a dedicated port (or QGPSLOC over AT).
 * These parsers turn `$GxGGA` (altitude/quality/sats) and `$GxRMC`
 * (position/speed/course/validity) into a typed fix. No hardware — fully
 * host-unit-testable. Latitude/longitude are converted to signed decimal
 * degrees; speed to km/h.
 */

namespace cellular {

struct GpsFix {
    bool        valid = false;     ///< a usable 2D/3D position was parsed
    std::string quality;           ///< "none" / "2d" / "3d"
    double      lat = 0.0;         ///< decimal degrees, +N / -S
    double      lon = 0.0;         ///< decimal degrees, +E / -W
    double      alt_m = 0.0;       ///< altitude above MSL, metres (GGA)
    double      speed_kmh = 0.0;   ///< ground speed, km/h (RMC)
    double      course_deg = 0.0;  ///< course over ground, degrees (RMC)
    int         sats = 0;          ///< satellites used (GGA)
    std::string utc;               ///< raw UTC hhmmss[.sss] field
};

/// Validate the `*HH` XOR checksum of a full NMEA sentence (between '$' and '*').
bool nmea_checksum_ok(const std::string& sentence);

/// Parse a `$--GGA` sentence into `out` (lat/lon/alt/sats/quality). Returns
/// false on a malformed sentence or bad checksum; `out` is updated in place so
/// a GGA+RMC pair can be merged into one fix.
bool parse_gga(const std::string& sentence, GpsFix& out);

/// Parse a `$--RMC` sentence into `out` (lat/lon/speed/course/validity).
bool parse_rmc(const std::string& sentence, GpsFix& out);

} // namespace cellular

#endif /*__cellular_nmea_parser_hpp__*/
