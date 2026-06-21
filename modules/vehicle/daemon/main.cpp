/// iot-vehicled — vehicle telemetry over CAN (ISO 15765-4 / OBD-II) producer.
///
/// Reactor-driven (ACE): opens a SocketCAN interface, polls OBD-II Mode 01 PIDs
/// on a timer, decodes them via the pure vehicle::obd core, and publishes
/// `vehicle.*` to the data-store (volatile live keys). Config keys
/// (vehicle.can.iface / vehicle.poll.interval.ms) override the argv defaults
/// when present in ds. See apps/docs/tdd-vehicle-telemetry.md.

#include <cstdlib>
#include <iostream>
#include <string>

#include "vehicle_client.hpp"

namespace {

void usage() {
    std::cout <<
        "iot-vehicled — CAN/OBD-II vehicle telemetry producer\n"
        "\n"
        "Usage: iot-vehicled [--ds-sock=PATH] [--iface=can0] [--interval=MS] [--help]\n"
        "\n"
        "  --ds-sock=PATH   ds-server unix socket (default: ds built-in).\n"
        "  --iface=NAME     SocketCAN interface (default can0;\n"
        "                   overridden by vehicle.can.iface in ds).\n"
        "  --interval=MS    full-poll cadence in ms (default 1000 /\n"
        "                   vehicle.poll.interval.ms).\n"
        "  --help           show this and exit.\n";
}

} // namespace

int main(int argc, char** argv) {
    vehicle::VehicleClient::Config cfg;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if      (a.rfind("--ds-sock=", 0)  == 0) cfg.ds_sock = a.substr(10);
            else if (a.rfind("--iface=", 0)    == 0) cfg.iface = a.substr(8);
            else if (a.rfind("--interval=", 0) == 0) cfg.interval_ms = static_cast<unsigned>(std::stoul(a.substr(11)));
            else if (a == "--help" || a == "-h")     { usage(); return 0; }
            else {
                std::cerr << "iot-vehicled: unknown argument '" << a << "'\n";
                usage();
                return 2;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "iot-vehicled: bad argument: " << e.what() << "\n";
        return 2;
    }
    if (cfg.interval_ms == 0) cfg.interval_ms = 1000;

    vehicle::VehicleClient client(std::move(cfg));
    return client.run();
}
