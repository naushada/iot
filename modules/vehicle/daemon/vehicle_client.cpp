#include "vehicle_client.hpp"

#include <ace/Log_Msg.h>
#include <ace/Reactor.h>
#include <ace/Time_Value.h>

#include "data_store/value.hpp"

#include "obd_pid.hpp"

namespace vehicle {

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

void VehicleClient::on_frame(std::uint32_t id, const std::uint8_t* data, std::uint8_t dlc) {
    if (id < obd::kEcuResponseIdBase || id > (obd::kEcuResponseIdBase + 7)) return;
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

    ACE_DEBUG((LM_INFO,
        ACE_TEXT("%D [veh] up: iface=%C poll=%ums (%u PIDs, %ums/PID)\n"),
        m_cfg.iface.c_str(), m_cfg.interval_ms,
        static_cast<unsigned>(m_pids.size()), tick_ms));

    ACE_Reactor::instance()->run_reactor_event_loop();
    return 0;
}

} // namespace vehicle
