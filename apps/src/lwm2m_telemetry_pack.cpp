#include "lwm2m_telemetry_pack.hpp"

#include <cmath>

namespace lwm2m { namespace telemetry {

namespace {
/// A value is "integral" (emit as SenML int, not float) when it has no
/// fractional part and fits a 53-bit double-exact integer range. Vehicle
/// signals are small, so the range check is academic but keeps it honest.
bool is_integral(double v) {
    double intpart = 0.0;
    return std::modf(v, &intpart) == 0.0 && std::fabs(v) < 9.0e15;
}
} // namespace

std::vector<senml::Record> build_pack(const std::string& basePath,
                                      const std::vector<Sample>& samples) {
    std::vector<senml::Record> out;
    if (samples.empty()) return out;

    const double bt = samples.front().timeUnix;
    for (const auto& s : samples) {
        for (const auto& kv : s.values) {
            senml::Record r;
            // bn + bt are materialised on every record; the codec emits them
            // on the wire for record 0 only (and validates consistency).
            r.baseName    = basePath;
            r.hasBaseTime = true;
            r.baseTime    = bt;
            r.name        = kv.first;
            r.kind        = senml::ValueKind::Numeric;
            r.numericValue = kv.second;
            r.isFloat     = !is_integral(kv.second);
            r.hasTime     = true;
            r.time        = s.timeUnix - bt;   // offset from base time
            out.push_back(std::move(r));
        }
    }
    return out;
}

std::vector<Sample> parse_pack(const std::vector<senml::Record>& records) {
    std::vector<Sample> out;
    for (const auto& r : records) {
        if (r.kind != senml::ValueKind::Numeric) continue;   // telemetry is numeric
        const double t = r.effectiveTime();
        // build_pack emits a sample's readings consecutively at one time, so a
        // change in effective time starts a new Sample.
        if (out.empty() || out.back().timeUnix != t) {
            Sample s;
            s.timeUnix = t;
            out.push_back(std::move(s));
        }
        out.back().values.emplace_back(r.name, r.numericValue);
    }
    return out;
}

}} // namespace lwm2m::telemetry
