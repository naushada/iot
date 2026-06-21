/// iot-mqttd — mirror device telemetry to an operator-owned MQTT broker.
///
/// Reactor-driven (ACE): an ACE timer drives libmosquitto's network loop, and a
/// second timer publishes vehicle.* to `<iot.serial>/<mqtt.topic.suffix>`. A
/// SEPARATE, VPN-independent plane (device → broker over the WAN). Parks until
/// mqtt.broker.host is configured. See apps/docs/tdd-vehicle-telemetry.md.

#include <cstdlib>
#include <iostream>
#include <string>

#include "mqtt_mirror.hpp"

namespace {

void usage() {
    std::cout <<
        "iot-mqttd — MQTT telemetry mirror\n"
        "\n"
        "Usage: iot-mqttd [--ds-sock=PATH] [--interval=MS] [--help]\n"
        "\n"
        "  --ds-sock=PATH   ds-server unix socket (default: ds built-in).\n"
        "  --interval=MS    telemetry publish cadence (default 1000).\n"
        "  --help           show this and exit.\n"
        "\n"
        "Broker host/port/creds + topic + mirror toggle come from mqtt.* in ds\n"
        "(set via the device-ui). The daemon parks until mqtt.broker.host is set.\n";
}

} // namespace

int main(int argc, char** argv) {
    mqtt::MqttMirror::Config cfg;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if      (a.rfind("--ds-sock=", 0)  == 0) cfg.ds_sock = a.substr(10);
            else if (a.rfind("--interval=", 0) == 0) cfg.publish_ms = static_cast<unsigned>(std::stoul(a.substr(11)));
            else if (a == "--help" || a == "-h")     { usage(); return 0; }
            else {
                std::cerr << "iot-mqttd: unknown argument '" << a << "'\n";
                usage();
                return 2;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "iot-mqttd: bad argument: " << e.what() << "\n";
        return 2;
    }
    if (cfg.publish_ms == 0) cfg.publish_ms = 1000;

    mqtt::MqttMirror client(std::move(cfg));
    return client.run();
}
