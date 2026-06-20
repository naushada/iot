/// cellular-client — mangOH Yellow cellular WAN + GPS producer daemon.
///
/// Reactor-driven (ACE): opens the modem AT tty (and optional GNSS NMEA tty),
/// polls the modem on a timer, and publishes cell.* / gps.* to the data-store.
/// Config keys (cell.modem.tty / cell.gps.tty / cell.apn / cell.poll.interval.sec
/// / cell.gps.enable) override the argv defaults when present in ds.

#include <cstdlib>
#include <iostream>
#include <string>

#include "cellular_client.hpp"

namespace {

void usage() {
    std::cout <<
        "cellular-client — mangOH Yellow cellular WAN + GPS producer\n"
        "\n"
        "Usage: cellular-client [--ds-sock=PATH] [--modem-tty=DEV] [--gps-tty=DEV]\n"
        "                       [--apn=NAME] [--interval=SECS] [--no-gps] [--help]\n"
        "\n"
        "  --ds-sock=PATH    ds-server unix socket (default: ds built-in).\n"
        "  --modem-tty=DEV   AT control device (default /dev/ttyUSB2;\n"
        "                    overridden by cell.modem.tty in ds).\n"
        "  --gps-tty=DEV     NMEA GNSS device (default: cell.gps.tty in ds).\n"
        "  --apn=NAME        data-context APN (default: cell.apn in ds).\n"
        "  --interval=SECS   poll cadence (default 30 / cell.poll.interval.sec).\n"
        "  --no-gps          disable GNSS reads.\n"
        "  --help            show this and exit.\n";
}

} // namespace

int main(int argc, char** argv) {
    cellular::CellularClient::Config cfg;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if      (a.rfind("--ds-sock=", 0)   == 0) cfg.ds_sock = a.substr(10);
            else if (a.rfind("--modem-tty=", 0) == 0) cfg.modem_tty = a.substr(12);
            else if (a.rfind("--gps-tty=", 0)   == 0) cfg.gps_tty = a.substr(10);
            else if (a.rfind("--apn=", 0)       == 0) cfg.apn = a.substr(6);
            else if (a.rfind("--interval=", 0)  == 0) cfg.interval_sec = static_cast<unsigned>(std::stoul(a.substr(11)));
            else if (a == "--no-gps")                 cfg.gps_enable = false;
            else if (a == "--help" || a == "-h")      { usage(); return 0; }
            else {
                std::cerr << "cellular-client: unknown argument '" << a << "'\n";
                usage();
                return 2;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "cellular-client: bad argument: " << e.what() << "\n";
        return 2;
    }
    if (cfg.interval_sec == 0) cfg.interval_sec = 30;

    cellular::CellularClient client(std::move(cfg));
    return client.run();
}
