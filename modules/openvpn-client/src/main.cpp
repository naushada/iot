/// openvpn-client — v0 scaffold (L12/D2).
///
/// Usage:
///   openvpn-client [--ds-sock=PATH] [--help]
///
/// Today: connects to ds-server, dumps every known vpn.* key (whether
/// set or unset), exits. D3 layers a proper DsBridge + watch over
/// the bare get; D4/D5/D6 add the mgmt protocol parser, openvpn(8)
/// subprocess, and the connect → first-PUSH_REPLY → write-back loop.

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "openvpn_client/client.hpp"

namespace {

void usage() {
    std::cout <<
        "openvpn-client (L12/D2 scaffold)\n"
        "\n"
        "Usage: openvpn-client [--ds-sock=PATH] [--help]\n"
        "\n"
        "  --ds-sock=PATH   ds-server unix socket; defaults to ds-server's\n"
        "                   built-in default (/var/run/iot/data_store.sock).\n"
        "  --help           show this and exit.\n"
        "\n"
        "Today this binary connects to ds-server, dumps every known vpn.*\n"
        "key, and exits. The full lifecycle lands in L12/D3..D6.\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string sock;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--ds-sock=", 0) == 0) {
            sock = a.substr(10);
        } else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            std::cerr << "openvpn-client: unknown argument '" << a << "'\n";
            usage();
            return 2;
        }
    }

    auto rs = openvpn_client::v0_dump_vpn_keys(sock);
    return rs.ok ? 0 : 1;
}
