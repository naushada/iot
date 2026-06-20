/// iot-sensord — mangOH Yellow sensor producer daemon entry.
///
/// Maps the I2C bus (root / CAP_SYS_RAWIO), samples the onboard sensors and
/// publishes iot.sensor.* into the data-store on a fixed interval.

#include <cstdlib>
#include <iostream>
#include <string>

#include "sensord.hpp"

namespace {

void usage() {
    std::cout <<
        "iot-sensord — mangOH Yellow sensor producer\n"
        "\n"
        "Usage: iot-sensord [--ds-sock=PATH] [--i2c-dev=DEV] [--mmio]\n"
        "                   [--interval=SECS] [--once]\n"
        "                   [--bme=ADDR] [--imu=ADDR] [--light=ADDR] [--help]\n"
        "\n"
        "  --ds-sock=PATH   ds-server unix socket (default: ds built-in).\n"
        "  --i2c-dev=DEV    kernel i2c-dev node (default /dev/i2c-1). Preferred —\n"
        "                   no CAP_SYS_RAWIO. Falls back to BSC /dev/mem if absent.\n"
        "  --mmio           force the BSC /dev/mem register transport (skip i2c-dev).\n"
        "  --interval=SECS  sample cadence (default 10).\n"
        "  --once           sample once, publish, and exit.\n"
        "  --bme=ADDR       BME680 I2C address (default 0x76).\n"
        "  --imu=ADDR       BMI160 I2C address (default 0x68).\n"
        "  --light=ADDR     light-sensor I2C address (default 0x44).\n"
        "  --help           show this and exit.\n";
}

std::uint8_t parse_addr(const std::string& s) {
    return static_cast<std::uint8_t>(std::stoul(s, nullptr, 0));   // 0 → honor 0x..
}

} // namespace

int main(int argc, char** argv) {
    sensors::Options opt;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if      (a.rfind("--ds-sock=", 0)  == 0) opt.ds_sock = a.substr(10);
            else if (a.rfind("--i2c-dev=", 0)  == 0) opt.i2c_dev = a.substr(10);
            else if (a == "--mmio")                  opt.i2c_dev.clear();
            else if (a.rfind("--interval=", 0) == 0) opt.interval_sec = static_cast<std::uint32_t>(std::stoul(a.substr(11)));
            else if (a.rfind("--bme=", 0)      == 0) opt.bme_addr = parse_addr(a.substr(6));
            else if (a.rfind("--imu=", 0)      == 0) opt.imu_addr = parse_addr(a.substr(6));
            else if (a.rfind("--light=", 0)    == 0) opt.light_addr = parse_addr(a.substr(8));
            else if (a == "--once")                  opt.once = true;
            else if (a == "--help" || a == "-h")     { usage(); return 0; }
            else {
                std::cerr << "iot-sensord: unknown argument '" << a << "'\n";
                usage();
                return 2;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "iot-sensord: bad argument: " << e.what() << "\n";
        return 2;
    }
    if (opt.interval_sec == 0) opt.interval_sec = 10;
    return sensors::run(opt);
}
