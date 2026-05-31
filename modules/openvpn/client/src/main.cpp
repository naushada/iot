/// openvpn-client — daemon entry (L12/D6).
///
/// Usage:
///   openvpn-client [--ds-sock=PATH] [--openvpn=PATH] [--dump] [--once] [--help]
///
/// Default (no --dump): full lifecycle — spawn openvpn(8), connect
/// to its mgmt socket, route STATE / PUSH_REPLY into vpn.* writes.
/// --once exits after the first PUSH_REPLY (useful for smoke tests).
/// --dump just reads the current vpn.* snapshot and exits.

#include <cstdlib>
#include <iostream>
#include <string>

#include "client.hpp"

namespace {

void usage() {
    std::cout <<
        "openvpn-client (L12/D6)\n"
        "\n"
        "Usage: openvpn-client [--ds-sock=PATH] [--openvpn=PATH] [--dump] [--once] [--help]\n"
        "\n"
        "  --ds-sock=PATH   ds-server unix socket; defaults to ds-server's\n"
        "                   built-in default (/var/run/iot/data_store.sock).\n"
        "  --openvpn=PATH   path to openvpn(8); defaults to /usr/sbin/openvpn.\n"
        "  --dump           snapshot vpn.* and exit (diagnostic; no subprocess).\n"
        "  --once           exit after the first PUSH_REPLY (smoke / one-shot).\n"
        "  --help           show this and exit.\n";
}

} // namespace

int main(int argc, char** argv) {
    std::string sock;
    std::string openvpn_path = "/usr/sbin/openvpn";
    bool        dump = false;
    bool        once = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--ds-sock=", 0) == 0)        sock = a.substr(10);
        else if (a.rfind("--openvpn=", 0) == 0)   openvpn_path = a.substr(10);
        else if (a == "--dump")                   dump = true;
        else if (a == "--once")                   once = true;
        else if (a == "--help" || a == "-h")      { usage(); return 0; }
        else {
            std::cerr << "openvpn-client: unknown argument '" << a << "'\n";
            usage();
            return 2;
        }
    }

    auto rs = dump
            ? openvpn_client::v0_dump_vpn_keys(sock)
            : openvpn_client::run_daemon(sock, openvpn_path, once);
    return rs.ok ? 0 : 1;
}
