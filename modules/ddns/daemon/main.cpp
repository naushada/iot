/// iot-ddnsd — device-side Dynamic DNS updater daemon.
///
/// Reactor-driven (ACE): connects to ds, watches the ddns.* config + the active
/// WAN IP, and on a timer keeps a public DNS A record pointed at the device's
/// current public IPv4 through a pluggable provider backend (dyndns2 / duckdns
/// / cloudflare / route53). Config keys override the argv defaults and hot-apply
/// on change. Ships disabled by default. See apps/docs/tdd-ddns.md.

#include <cstdlib>
#include <iostream>
#include <string>

#include "ddns_client.hpp"

namespace {

void usage() {
    std::cout <<
        "iot-ddnsd — device-side Dynamic DNS updater\n"
        "\n"
        "Usage: iot-ddnsd [--ds-sock=PATH] [--provider=NAME] [--hostname=FQDN]\n"
        "                 [--interval=SECS] [--enable] [--help]\n"
        "\n"
        "  --ds-sock=PATH    ds-server unix socket (default: ds built-in).\n"
        "  --provider=NAME   dyndns2|duckdns|cloudflare|route53 (default: ddns.provider).\n"
        "  --hostname=FQDN   record to keep updated (default: ddns.hostname).\n"
        "  --interval=SECS   detect/poll cadence (default 300 / ddns.interval).\n"
        "  --enable          start enabled even if ddns.enabled is unset.\n"
        "  --help            show this and exit.\n"
        "\n"
        "All flags are argv defaults; the ddns.* ds keys override them and\n"
        "hot-apply on change.\n";
}

} // namespace

int main(int argc, char** argv) {
    ddns::DdnsClient::Config cfg;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if      (a.rfind("--ds-sock=", 0)  == 0) cfg.ds_sock = a.substr(10);
            else if (a.rfind("--provider=", 0) == 0) cfg.provider = a.substr(11);
            else if (a.rfind("--hostname=", 0) == 0) cfg.hostname = a.substr(11);
            else if (a.rfind("--interval=", 0) == 0) cfg.interval_sec = static_cast<unsigned>(std::stoul(a.substr(11)));
            else if (a == "--enable")                cfg.enabled = true;
            else if (a == "--help" || a == "-h")     { usage(); return 0; }
            else {
                std::cerr << "iot-ddnsd: unknown argument '" << a << "'\n";
                usage();
                return 2;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "iot-ddnsd: bad argument: " << e.what() << "\n";
        return 2;
    }
    if (cfg.interval_sec == 0) cfg.interval_sec = 300;

    ddns::DdnsClient client(std::move(cfg));
    return client.run();
}
