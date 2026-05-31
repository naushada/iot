/// net-router — daemon entry (L13/D2 scaffold).
///
/// Usage:
///   net-router [--ds-sock=PATH] [--dump] [--help]
///
/// Today (D2 scaffold): connects to ds-server, dumps every known
/// net.* key (whether set or unset), warns if net.lwm2m.target_ip
/// is missing, exits. D3..D7 add the DsBridge, nftables rule
/// generator, ip route metrics, iface monitor, and the
/// IOT_ROLE=net systemd integration.

#include <cstdlib>
#include <iostream>
#include <string>

#include "router.hpp"

namespace {

void usage() {
    std::cout <<
        "net-router (L13/D2 scaffold)\n"
        "\n"
        "Usage: net-router [--ds-sock=PATH] [--dump] [--help]\n"
        "\n"
        "  --ds-sock=PATH   ds-server unix socket; defaults to ds-server's\n"
        "                   built-in default (/var/run/iot/data_store.sock).\n"
        "  --dump           snapshot net.* and exit (current default).\n"
        "  --help           show this and exit.\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string sock;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a.rfind("--ds-sock=", 0) == 0) sock = a.substr(10);
        else if (a == "--dump")                 { /* default mode */ }
        else if (a == "--help" || a == "-h")    { usage(); return 0; }
        else {
            std::cerr << "net-router: unknown argument '" << a << "'\n";
            usage();
            return 2;
        }
    }

    auto rs = net_router::v0_dump_net_keys(sock);
    return rs.ok ? 0 : 1;
}
