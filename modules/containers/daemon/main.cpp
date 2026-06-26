/// iot-containerd — multi-container runtime shim (crun-backed) entry.
///
/// Reactor-driven (ACE): connects to the data-store, publishes the
/// container.instances JSON array, and watches the container.cmd.request command
/// envelope the device-ui bumps (action=pull|run|stop|remove, routed by name).
/// Pulls images from an OCI/Docker registry, overlay-mounts the layers, and
/// drives crun create/start/stop per named container. See
/// apps/docs/tdd-device-containers.md §13.

#include <cstdlib>
#include <iostream>
#include <string>

#include "containerd.hpp"

namespace {

void usage() {
    std::cout <<
        "iot-containerd — multi-container runtime shim (crun-backed)\n"
        "\n"
        "Usage: iot-containerd [--ds-sock=PATH] [--root=DIR] [--run=DIR] [--help]\n"
        "\n"
        "  --ds-sock=PATH   ds-server unix socket (default: ds built-in).\n"
        "  --root=DIR       persistent image/layer store (default\n"
        "                   /var/lib/iot-containers).\n"
        "  --run=DIR        ephemeral overlay/bundle dir (default\n"
        "                   /run/iot-containers).\n"
        "  --help           show this and exit.\n";
}

} // namespace

int main(int argc, char** argv) {
    containers::Containerd::Config cfg;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if      (a.rfind("--ds-sock=", 0) == 0) cfg.ds_sock = a.substr(10);
            else if (a.rfind("--root=", 0)    == 0) cfg.root = a.substr(7);
            else if (a.rfind("--run=", 0)     == 0) cfg.run = a.substr(6);
            else if (a == "--help" || a == "-h")    { usage(); return 0; }
            else {
                std::cerr << "iot-containerd: unknown argument '" << a << "'\n";
                usage();
                return 2;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "iot-containerd: bad argument: " << e.what() << "\n";
        return 2;
    }

    containers::Containerd daemon(std::move(cfg));
    return daemon.run();
}
