#include "mqtt_mirror.hpp"

#include <cstring>
#include <string>
#include <vector>

#include <mosquitto.h>

#include <ace/Log_Msg.h>
#include <ace/Reactor.h>
#include <ace/Time_Value.h>

#include "data_store/log_buffer.hpp"
#include "data_store/stats_publisher.hpp"
#include "data_store/value.hpp"

namespace mqtt {

// Owns the libmosquitto handle via std::unique_ptr (no raw owning pointer).
void MosqDeleter::operator()(::mosquitto* m) const noexcept { if (m) mosquitto_destroy(m); }

// Captures this daemon's ACE log output to log.mqttd.text and applies the
// per-daemon level from log.level.mqtt (falls back to log.level). See
// data_store::LogBuffer.
static data_store::LogBuffer g_log("mqttd", "log.mqttd.text", "log.level.mqtt");

namespace {
const void* const kLoopAct = reinterpret_cast<const void*>(1);  // mosquitto_loop
const void* const kPubAct  = reinterpret_cast<const void*>(2);  // publish telemetry

// vehicle.* keys mirrored to the broker. "vehicle." (8 chars) is stripped for
// the JSON field name; vehicle.dtc is already a JSON array (embedded raw).
const char* const kVehicleKeys[] = {
    "vehicle.link", "vehicle.speed", "vehicle.rpm", "vehicle.coolant",
    "vehicle.throttle", "vehicle.load", "vehicle.fuel", "vehicle.iat",
    "vehicle.maf", "vehicle.dtc",
};
} // namespace

MqttMirror::~MqttMirror() {
    if (m_mosq) {
        mosquitto_disconnect(m_mosq.get());
        m_mosq.reset();   // deleter → mosquitto_destroy
    }
    mosquitto_lib_cleanup();
}

std::string MqttMirror::read_key(const char* key) {
    std::vector<data_store::Client::GetResult> got;
    if (m_ds.get({std::string(key)}, got).ok && !got.empty() && got[0].has_value)
        if (auto s = data_store::to_string(got[0].value)) return *s;
    return std::string();
}

void MqttMirror::reload_config() {
    m_host = read_key("mqtt.broker.host");
    std::vector<data_store::Client::GetResult> got;
    if (m_ds.get({std::string("mqtt.broker.port")}, got).ok && !got.empty() && got[0].has_value)
        if (auto n = data_store::to_int32(got[0].value)) m_port = *n;
    if (m_port <= 0) m_port = 1883;
    got.clear();
    if (m_ds.get({std::string("mqtt.qos")}, got).ok && !got.empty() && got[0].has_value)
        if (auto n = data_store::to_int32(got[0].value)) m_qos = *n;
    got.clear();
    if (m_ds.get({std::string("mqtt.mirror.enable")}, got).ok && !got.empty() && got[0].has_value)
        if (auto b = data_store::to_bool(got[0].value)) m_mirror = *b;

    std::string serial = read_key("iot.serial");
    std::string suffix = read_key("mqtt.topic.suffix");
    if (suffix.empty()) suffix = "telemetry";
    if (serial.empty()) serial = "device";
    m_topic = serial + "/" + suffix;
}

void MqttMirror::ensure_connected() {
    if (m_connected) return;
    reload_config();
    if (m_host.empty()) return;   // unconfigured → stay parked (effectively off)

    if (!m_mosq) {
        std::string cid = read_key("mqtt.client.id");
        m_mosq.reset(mosquitto_new(cid.empty() ? nullptr : cid.c_str(), true, this));
        if (!m_mosq) {
            ACE_ERROR((LM_ERROR, ACE_TEXT("%D [mqtt] mosquitto_new failed\n")));
            return;
        }
        std::string user = read_key("mqtt.broker.user");
        std::string pass = read_key("mqtt.broker.pass");
        if (!user.empty())
            mosquitto_username_pw_set(m_mosq.get(), user.c_str(),
                                      pass.empty() ? nullptr : pass.c_str());
    }

    int rc = mosquitto_connect(m_mosq.get(), m_host.c_str(), m_port, 60);
    if (rc == MOSQ_ERR_SUCCESS) {
        m_connected = true;
        ACE_DEBUG((LM_INFO, ACE_TEXT("%D [mqtt] connected %C:%d topic=%C\n"),
                   m_host.c_str(), m_port, m_topic.c_str()));
    } else {
        ACE_ERROR((LM_WARNING,
                   ACE_TEXT("%D [mqtt] connect %C:%d failed rc=%d (retry)\n"),
                   m_host.c_str(), m_port, rc));
    }
}

void MqttMirror::publish_telemetry() {
    if (!m_connected || !m_mosq) return;
    // Re-read the mirror toggle live so an operator flip applies without restart.
    std::vector<data_store::Client::GetResult> g;
    if (m_ds.get({std::string("mqtt.mirror.enable")}, g).ok && !g.empty() && g[0].has_value)
        if (auto b = data_store::to_bool(g[0].value)) m_mirror = *b;
    if (!m_mirror) return;

    std::string json = "{";
    bool first = true;
    for (const char* k : kVehicleKeys) {
        std::string v = read_key(k);
        if (v.empty()) continue;
        if (!first) json += ",";
        first = false;
        const char* field = k + 8;          // strip "vehicle."
        json += "\"";
        json += field;
        json += "\":";
        if (std::strcmp(field, "dtc") == 0) {
            json += v;                        // already a JSON array
        } else {
            json += "\"";
            json += v;
            json += "\"";
        }
    }
    json += "}";

    mosquitto_publish(m_mosq.get(), nullptr, m_topic.c_str(),
                      static_cast<int>(json.size()), json.data(), m_qos, true /*retain*/);
}

int MqttMirror::handle_timeout(const ACE_Time_Value&, const void* act) {
    if (act == kLoopAct) {
        if (m_mosq && m_connected) {
            int rc = mosquitto_loop(m_mosq.get(), 0, 1);
            if (rc != MOSQ_ERR_SUCCESS) {
                if (mosquitto_reconnect(m_mosq.get()) != MOSQ_ERR_SUCCESS) m_connected = false;
            }
        } else {
            // Parked / disconnected: retry every ~5 s (not every 100 ms tick),
            // so an unconfigured device doesn't hammer ds.
            if (++m_loop_ticks % 50 == 0) ensure_connected();
        }
    } else if (act == kPubAct) {
        publish_telemetry();
    }
    return 0;
}

int MqttMirror::run() {
    if (!m_ds.connect(m_cfg.ds_sock).ok) {
        ACE_ERROR_RETURN((LM_ERROR, ACE_TEXT("%D [mqtt] ds connect failed\n")), 1);
    }
    mosquitto_lib_init();
    reload_config();

    // Loop timer (100 ms): drives mosquitto network read/write + reconnect.
    ACE_Reactor::instance()->schedule_timer(this, kLoopAct,
        ACE_Time_Value(1), ACE_Time_Value(0, 100 * 1000));
    // Publish timer: mirror vehicle.* to the broker at the configured cadence.
    ACE_Time_Value pub(m_cfg.publish_ms / 1000, (m_cfg.publish_ms % 1000) * 1000);
    ACE_Reactor::instance()->schedule_timer(this, kPubAct, ACE_Time_Value(2), pub);

    // ACE is initialised now (the reactor was used above): capture logs to
    // log.mqttd.text, apply log.level.mqtt, and drive the flush timer.
    g_log.start();
    g_log.apply_level(m_ds);
    g_log.open(m_ds, 5, 1);
    // Self-report for the Services page.
    m_ds.set(std::string("services.mqtt.state"), data_store::Value{std::string("running")});

    // L22 resource telemetry → services.mqtt.{cpu,mem,fd,threads}. Singleton
    // reactor is pumped in-thread here, so schedule on it (no extra thread).
    data_store::StatsPublisher stats("services.mqtt",
        [this](const std::vector<data_store::KV>& kv) { m_ds.set(kv); });
    stats.open(data_store::StatsPublisher::STATS_FLUSH_SEC, false);

    ACE_DEBUG((LM_INFO,
        ACE_TEXT("%D [mqtt] up: broker=%C:%d topic=%C (parks until host set)\n"),
        m_host.empty() ? "(unset)" : m_host.c_str(), m_port, m_topic.c_str()));

    ACE_Reactor::instance()->run_reactor_event_loop();
    return 0;
}

} // namespace mqtt
