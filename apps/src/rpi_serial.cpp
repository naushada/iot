#include "rpi_serial.hpp"

#include <fstream>
#include <sstream>

namespace iot {

namespace {

/// Trim ASCII whitespace and trailing NULs from both ends. The
/// device-tree serial-number property is a NUL-terminated string, so the
/// raw read often carries a trailing '\0' and/or newline.
std::string trim(const std::string& in) {
    auto is_junk = [](char c) {
        return c == '\0' || c == '\n' || c == '\r' ||
               c == ' '  || c == '\t';
    };
    std::size_t b = 0, e = in.size();
    while (b < e && is_junk(in[b])) ++b;
    while (e > b && is_junk(in[e - 1])) --e;
    return in.substr(b, e - b);
}

std::string read_whole_file(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

/// Parse the `Serial` field from /proc/cpuinfo content. Lines look like:
///   Serial          : 0000000012345678
/// Leading zeros are part of the value and are preserved.
std::string parse_cpuinfo_serial(const std::string& content) {
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        // Case-sensitive "Serial" is what the kernel emits.
        if (line.rfind("Serial", 0) != 0) continue;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        return trim(line.substr(colon + 1));
    }
    return {};
}

} // namespace

std::string read_rpi_serial(const std::string& devtree_path,
                            const std::string& cpuinfo_path) {
    // Preferred: device-tree serial-number (works on RPi 4/CM4 and most
    // modern firmware, independent of the cpuinfo "Serial" quirks).
    if (auto dt = trim(read_whole_file(devtree_path)); !dt.empty())
        return dt;

    // Fallback: /proc/cpuinfo "Serial" line.
    if (auto cs = parse_cpuinfo_serial(read_whole_file(cpuinfo_path));
        !cs.empty())
        return cs;

    return {};
}

bool is_rpi(const std::string& model_path) {
    const std::string model = read_whole_file(model_path);
    return model.find("Raspberry Pi") != std::string::npos;
}

} // namespace iot
