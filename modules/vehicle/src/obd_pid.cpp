#include "obd_pid.hpp"

#include <cstdio>

namespace vehicle {
namespace obd {

std::array<std::uint8_t, 8> build_request(std::uint8_t pid, std::uint8_t mode) {
    std::array<std::uint8_t, 8> f;
    f.fill(kPad);
    f[0] = 0x02;  // single frame, 2 data bytes follow (mode + pid)
    f[1] = mode;
    f[2] = pid;
    return f;
}

ObdValue decode_response(const std::uint8_t* d, std::size_t len) {
    ObdValue v;
    // Need at least [len, 0x41, pid, A].
    if (!d || len < 4) return v;
    // Mode 01 positive response service id is 0x41 (mode | 0x40).
    if (d[1] != (kModeCurrentData | kPositiveReplyOffset)) return v;

    const std::uint8_t pid   = d[2];
    const std::uint8_t A     = d[3];
    const bool         haveB = (len >= 5);
    const std::uint8_t B     = haveB ? d[4] : 0;
    v.pid = pid;

    switch (pid) {
        case kPidEngineLoad:   v.value = A * 100.0 / 255.0;          v.unit = "%";    v.valid = true; break;
        case kPidCoolantTemp:  v.value = static_cast<int>(A) - 40;   v.unit = "C";    v.valid = true; break;
        case kPidRpm:          if (!haveB) return v;
                               v.value = (256.0 * A + B) / 4.0;      v.unit = "rpm";  v.valid = true; break;
        case kPidSpeed:        v.value = A;                          v.unit = "km/h"; v.valid = true; break;
        case kPidIntakeAirTmp: v.value = static_cast<int>(A) - 40;   v.unit = "C";    v.valid = true; break;
        case kPidMaf:          if (!haveB) return v;
                               v.value = (256.0 * A + B) / 100.0;    v.unit = "g/s";  v.valid = true; break;
        case kPidThrottle:     v.value = A * 100.0 / 255.0;          v.unit = "%";    v.valid = true; break;
        case kPidFuelLevel:    v.value = A * 100.0 / 255.0;          v.unit = "%";    v.valid = true; break;
        default:               break;  // unsupported PID → valid stays false
    }
    return v;
}

std::string to_ds_string(const ObdValue& v) {
    if (!v.valid) return std::string();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v.value);
    std::string s(buf);
    // Trim trailing zeros (and a dangling dot) so 1000.00 → "1000", 49.80 → "49.8".
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

std::string decode_dtc(std::uint8_t hi, std::uint8_t lo) {
    if (hi == 0 && lo == 0) return std::string();  // no DTC
    static const char kCat[4] = {'P', 'C', 'B', 'U'};
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%c%X%X%X%X",
                  kCat[(hi >> 6) & 0x03],
                  (hi >> 4) & 0x03,
                  hi & 0x0F,
                  (lo >> 4) & 0x0F,
                  lo & 0x0F);
    return std::string(buf);
}

}  // namespace obd
}  // namespace vehicle
