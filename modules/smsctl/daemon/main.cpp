/// iot-smsctld — authenticated device control over SMS.
///
/// Reactor-driven (ACE): consumes the MT-SMS envelope cellular-client publishes
/// (sms.version + sms.last.*), authenticates the sender against the device's own
/// users (auth.users.*), executes one of a fixed command allowlist — STATUS,
/// REBOOT, FACTORY-RESET, APN, RADIO RESTART, WIFI — and answers with a single
/// MO SMS. It never talks to the modem directly and holds no extra privilege:
/// privileged actions are armed as trigger files under /run/iot for the root
/// .path units. Ships disabled (smsctl.enabled=false).
/// See apps/docs/tdd-smsctl.md.

#include <iostream>
#include <string>

#include "smsctl_client.hpp"

namespace {

void usage() {
    std::cout <<
        "iot-smsctld — authenticated device control over SMS\n"
        "\n"
        "Usage: iot-smsctld [--ds-sock=PATH] [--help]\n"
        "\n"
        "  --ds-sock=PATH    ds-server unix socket (default: ds built-in).\n"
        "  --help            show this and exit.\n"
        "\n"
        "Behaviour is driven by the smsctl.* ds keys (enabled, allowed.numbers,\n"
        "session.ttl.sec, lockout.*), which hot-apply on change. The daemon is\n"
        "inert until smsctl.enabled=true.\n";
}

} // namespace

int main(int argc, char** argv) {
    smsctl::SmsCtlClient::Config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if      (a.rfind("--ds-sock=", 0) == 0) cfg.ds_sock = a.substr(10);
        else if (a == "--help" || a == "-h")    { usage(); return 0; }
        else {
            std::cerr << "iot-smsctld: unknown argument '" << a << "'\n";
            usage();
            return 2;
        }
    }

    smsctl::SmsCtlClient client(std::move(cfg));
    return client.run();
}
