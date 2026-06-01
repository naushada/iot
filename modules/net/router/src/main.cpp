/// net-router — daemon entry.
///
/// Modes:
///   --dump            snapshot net.* and exit (default with no other mode)
///   --daemon          run the FSM loop forever (systemd unit uses this)
///
/// All other args are wiring (socket path, nft binary, poll override).

#include <cstdlib>
#include <iostream>
#include <string>

#include "router.hpp"

namespace {

void usage() {
    std::cout <<
        "net-router\n"
        "\n"
        "Usage: net-router [--ds-sock=PATH] [--nft=PATH] [--poll=SECS]\n"
        "                  [--dump | --daemon] [--help]\n"
        "\n"
        "  --ds-sock=PATH  ds-server unix socket; defaults to ds-server's\n"
        "                  built-in default (/var/run/iot/data_store.sock).\n"
        "  --nft=PATH      nft(8) binary path (default: \"nft\" via $PATH).\n"
        "  --poll=SECS     override net.poll.interval.sec (default: use ds key).\n"
        "  --dump          snapshot net.* and exit (default if no mode).\n"
        "  --daemon        run the lifecycle FSM forever.\n"
        "  --help          show this and exit.\n";
}

enum class Mode { Dump, Daemon };

} // namespace

int main(int argc, char** argv) {
    std::string sock;
    std::string nft_path = "nft";
    unsigned    poll     = 0;
    Mode        mode     = Mode::Dump;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a.rfind("--ds-sock=", 0) == 0) sock = a.substr(10);
        else if (a.rfind("--nft=", 0)     == 0) nft_path = a.substr(6);
        else if (a.rfind("--poll=", 0)    == 0) poll = static_cast<unsigned>(std::stoul(a.substr(7)));
        else if (a == "--dump")                 mode = Mode::Dump;
        else if (a == "--daemon")               mode = Mode::Daemon;
        else if (a == "--help" || a == "-h")    { usage(); return 0; }
        else {
            std::cerr << "net-router: unknown argument '" << a << "'\n";
            usage();
            return 2;
        }
    }

    auto rs = (mode == Mode::Daemon)
            ? net_router::run_daemon(sock, nft_path, poll)
            : net_router::v0_dump_net_keys(sock);
    return rs.ok ? 0 : 1;
}
