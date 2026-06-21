#include "vehicle_client.hpp"

#include <ace/Log_Msg.h>
#include <ace/Reactor.h>
#include <ace/Time_Value.h>

#include "data_store/log_buffer.hpp"
#include "data_store/stats_publisher.hpp"
#include "data_store/value.hpp"

#include "obd_pid.hpp"

namespace vehicle {

// Captures this daemon's ACE log output to log.vehicled.text and applies the
// per-daemon level from log.level.vehicle (falls back to log.level).
static data_store::LogBuffer g_log("vehicled", "log.vehicled.text", "log.level.vehicle");

namespace {

// PID → data-store key. Only the supported v1 signals map; everything else is
// ignored. Keep in sync with schemas/vehicle.lua + vehicle::obd.
const char* ds_key_for_pid(std::uint8_t pid) {
    switch (pid) {
        case obd::kPidEngineLoad:   return "vehicle.load";
        case obd::kPidCoolantTemp:  return "vehicle.coolant";
        case obd::kPidRpm:          return "vehicle.rpm";
        case obd::kPidSpeed:        return "vehicle.speed";
        case obd::kPidIntakeAirTmp: return "vehicle.iat";
        case obd::kPidMaf:          return "vehicle.maf";
        case obd::kPidThrottle:     return "vehicle.throttle";
        case obd::kPidFuelLevel:    return "vehicle.fuel";
        default:                    return nullptr;
    }
}

} // namespace

void VehicleClient::load_config_from_ds() {
    std::vector<data_store::Client::GetResult> got;
    if (m_ds.get({std::string("vehicle.can.iface")}, got).ok && !got.empty() && got[0].has_value)
        if (auto s = data_store::to_string(got[0].value)) if (!s->empty()) m_cfg.iface = *s;
    got.clear();
    if (m_ds.get({std::string("vehicle.poll.interval.ms")}, got).ok && !got.empty() && got[0].has_value)
        if (auto n = data_store::to_int32(got[0].value)) if (*n > 0) m_cfg.interval_ms = static_cast<unsigned>(*n);
}

void VehicleClient::publish_link(const char* state) {
    if (m_link == state) return;     // only on change
    m_link = state;
    m_ds.set_volatile(std::string("vehicle.link"), data_store::Value{std::string(state)});
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [veh] link=%C\n"), state));
}

void VehicleClient::handle_dtc(const std::uint8_t* d, std::uint8_t dlc) {
    // Single-frame Mode 03 response: [pci, 0x43, dtc1_hi, dtc1_lo, ...]. The PCI
    // byte (d[0]) is the data-byte count, so we only parse DTC pairs within it
    // (the rest is CAN padding). Multi-DTC responses spanning >1 frame need
    // ISO-TP reassembly — a follow-up; v1 covers the common ≤2-DTC single frame.
    std::string json = "[";
    bool first = true;
    for (std::size_t i = 2; i + 1 <= d[0] && i + 1 < dlc; i += 2) {
        std::string code = obd::decode_dtc(d[i], d[i + 1]);
        if (code.empty()) continue;             // 0x0000 → no DTC / padding
        if (!first) json += ",";
        json += "\"" + code + "\"";
        first = false;
    }
    json += "]";
    if (json == m_dtc) return;                  // unchanged → no write
    m_dtc = json;
    m_any_reply = true;
    m_ds.set(std::string("vehicle.dtc"), data_store::Value{json});  // persistent
    ACE_DEBUG((LM_INFO, ACE_TEXT("%D [veh] DTCs=%C\n"), json.c_str()));
}

void VehicleClient::on_frame(std::uint32_t id, const std::uint8_t* data, std::uint8_t dlc) {
    if (id < obd::kEcuResponseIdBase || id > (obd::kEcuResponseIdBase + 7)) return;
    // Mode 03 (stored DTCs) positive response service id is 0x43.
    if (dlc >= 2 && data[1] == 0x43) { handle_dtc(data, dlc); return; }
    auto v = obd::decode_response(data, dlc);
    if (!v.valid) return;
    const char* key = ds_key_for_pid(v.pid);
    if (!key) return;
    m_any_reply = true;
    m_ds.set_volatile(std::string(key), data_store::Value{obd::to_ds_string(v)});
}

int VehicleClient::handle_timeout(const ACE_Time_Value&, const void*) {
    if (m_pids.empty() || !m_can) return 0;

    // A completed round with no reply means the bus is up but no ECU answered
    // (engine off / wrong bitrate); any reply this round means it's live.
    if (m_next == 0) {
        publish_link(m_any_reply ? "up" : "no-ecu");
        m_any_reply = false;
        // Poll stored DTCs (Mode 03) every ~30 rounds — they change rarely. The
        // request is a single frame [0x01, 0x03] (mode only, no PID).
        if (m_round++ % 30 == 0 && m_can) {
            std::array<std::uint8_t, 8> req;
            req.fill(obd::kPad);
            req[0] = 0x01;            // single frame, 1 data byte (mode)
            req[1] = obd::kModeDtc;   // 0x03
            m_can->send(obd::kFunctionalRequestId, req);
        }
    }

    const std::uint8_t pid = m_pids[m_next];
    if (m_can->send(obd::kFunctionalRequestId, obd::build_request(pid)) < 0) {
        publish_link("down");
    }
    m_next = (m_next + 1) % m_pids.size();
    return 0;
}

int VehicleClient::run() {
    if (!m_ds.connect(m_cfg.ds_sock).ok) {
        ACE_ERROR_RETURN((LM_ERROR, ACE_TEXT("%D [veh] ds connect failed\n")), 1);
    }
    load_config_from_ds();

    m_pids = {
        obd::kPidEngineLoad, obd::kPidCoolantTemp, obd::kPidRpm, obd::kPidSpeed,
        obd::kPidIntakeAirTmp, obd::kPidMaf, obd::kPidThrottle, obd::kPidFuelLevel,
    };

    m_can.reset(new CanSocket(
        [this](std::uint32_t id, const std::uint8_t* d, std::uint8_t dlc) { on_frame(id, d, dlc); }));
    if (m_can->open(m_cfg.iface, ACE_Reactor::instance()) == -1) {
        publish_link("down");
        ACE_ERROR_RETURN((LM_ERROR,
            ACE_TEXT("%D [veh] CAN iface %C unavailable (is it up? "
                     "`ip link set %C up type can`)\n"),
            m_cfg.iface.c_str(), m_cfg.iface.c_str()), 2);
    }

    // Spread the per-PID requests across the poll interval so we don't burst all
    // eight at once: one request per (interval / N) tick.
    unsigned tick_ms = m_cfg.interval_ms / static_cast<unsigned>(m_pids.size());
    if (tick_ms == 0) tick_ms = 1;
    ACE_Time_Value period(tick_ms / 1000, (tick_ms % 1000) * 1000);
    ACE_Reactor::instance()->schedule_timer(this, nullptr, ACE_Time_Value(1), period);

    // ACE is initialised now (reactor used above): capture logs to
    // log.vehicled.text, apply log.level.vehicle, drive the flush timer.
    g_log.start();
    g_log.apply_level(m_ds);
    g_log.open(m_ds, 5, 1);
    // Self-report for the Services page.
    m_ds.set(std::string("services.vehicle.state"), data_store::Value{std::string("running")});

    // L22 resource telemetry → services.vehicle.{cpu,mem,fd,threads}. This daemon
    // pumps the singleton reactor in-thread, so schedule on it (no extra thread).
    data_store::StatsPublisher stats("services.vehicle",
        [this](const std::vector<data_store::KV>& kv) { m_ds.set(kv); });
    stats.open(data_store::StatsPublisher::STATS_FLUSH_SEC, false);

    ACE_DEBUG((LM_INFO,
        ACE_TEXT("%D [veh] up: iface=%C poll=%ums (%u PIDs, %ums/PID)\n"),
        m_cfg.iface.c_str(), m_cfg.interval_ms,
        static_cast<unsigned>(m_pids.size()), tick_ms));

    ACE_Reactor::instance()->run_reactor_event_loop();
    return 0;
}

} // namespace vehicle
