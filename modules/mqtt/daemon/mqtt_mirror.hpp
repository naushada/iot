#ifndef __mqtt_mirror_hpp__
#define __mqtt_mirror_hpp__

#include <cstdint>
#include <string>

#include <ace/Event_Handler.h>

#include "data_store/client.hpp"

struct mosquitto;  // libmosquitto opaque handle (fwd-decl)

/// iot-mqttd — mirror device telemetry to an operator-owned MQTT broker.
///
/// A SEPARATE, VPN-independent plane: device → broker over the WAN (NOT the
/// cloud LwM2M pipeline, NOT the tunnel). Reactor-driven: an ACE timer drives
/// `mosquitto_loop()` (read/write/keepalive/reconnect in one call — simpler and
/// more robust than fd-reactor integration across reconnects), and a second
/// timer reads vehicle.* from ds, builds a JSON payload, and publishes it to
/// `<iot.serial>/<mqtt.topic.suffix>`. The daemon parks (no connection) until
/// mqtt.broker.host is configured, so it is effectively off by default.

namespace mqtt {

class MqttMirror : public ACE_Event_Handler {
    public:
        struct Config {
            std::string ds_sock;            ///< "" → ds default socket
            unsigned    publish_ms = 1000;  ///< telemetry publish cadence
        };

        explicit MqttMirror(Config cfg) : m_cfg(std::move(cfg)) {}
        ~MqttMirror() override;

        /// Connect ds, init libmosquitto, run the reactor. Process exit code.
        int run();

        /// Two timers, distinguished by `act`: kLoopAct drives mosquitto_loop,
        /// kPubAct drives the telemetry publish.
        int handle_timeout(const ACE_Time_Value&, const void* act) override;

    private:
        std::string read_key(const char* key);
        void        reload_config();        ///< re-read mqtt.* (host/creds/topic)
        void        ensure_connected();
        void        publish_telemetry();

        Config             m_cfg;
        data_store::Client m_ds;
        struct mosquitto*  m_mosq = nullptr;
        bool               m_connected = false;
        std::string        m_host;
        int                m_port = 1883;
        std::string        m_topic;          ///< <serial>/<suffix>
        bool               m_mirror = false;
        int                m_qos = 0;
        unsigned           m_loop_ticks = 0; ///< paces reconnect attempts while parked
};

} // namespace mqtt

#endif /*__mqtt_mirror_hpp__*/
