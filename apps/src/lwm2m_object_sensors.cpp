#include "lwm2m_object_sensors.hpp"

#include <utility>

namespace lwm2m { namespace objects {

namespace {

/// A read-only, observable IPSO value resource. Falls back to a static "0"
/// when no live reader is wired (board without the mangOH attached).
Resource value_res(std::uint32_t rid, const std::string& name,
                   std::function<std::string()> rd) {
    Resource r;
    r.rid = rid; r.name = name; r.type = ResourceType::Float;
    r.ops = Operations::R; r.observable = true;
    if (rd) {
        r.read = std::move(rd);
    } else {
        r.read = []() { return std::string("0"); };
    }
    return r;
}

/// IPSO RID 5701 "Sensor Units" — a fixed string per object.
Resource units_res(const std::string& units) {
    Resource r;
    r.rid = 5701; r.name = "Sensor Units"; r.type = ResourceType::String;
    r.ops = Operations::R;
    r.read = [units]() { return units; };
    return r;
}

ObjectDescriptor descriptor(std::uint32_t oid, const std::string& name,
                            const std::string& urn) {
    ObjectDescriptor d;
    d.oid = oid; d.name = name; d.urn = urn;
    d.mandatory = false; d.multipleInstance = true;   // IPSO objects are multi-instance
    return d;
}

/// Single-value IPSO sensor (5700 + 5701).
void add_scalar(ObjectStore& store, std::uint32_t oid, const std::string& name,
                const std::string& urn, const std::string& units,
                std::function<std::string()> rd) {
    ObjectDescriptor d = descriptor(oid, name, urn);
    ObjectInstance inst; inst.iid = 0;
    inst.resources[5700] = value_res(5700, "Sensor Value", std::move(rd));
    inst.resources[5701] = units_res(units);
    d.instances[0] = std::move(inst);
    store.add_object(std::move(d));
}

/// Three-axis IPSO sensor (5702/5703/5704 + 5701).
void add_triaxis(ObjectStore& store, std::uint32_t oid, const std::string& name,
                 const std::string& urn, const std::string& units,
                 std::function<std::string()> x,
                 std::function<std::string()> y,
                 std::function<std::string()> z) {
    ObjectDescriptor d = descriptor(oid, name, urn);
    ObjectInstance inst; inst.iid = 0;
    inst.resources[5702] = value_res(5702, "X Value", std::move(x));
    inst.resources[5703] = value_res(5703, "Y Value", std::move(y));
    inst.resources[5704] = value_res(5704, "Z Value", std::move(z));
    inst.resources[5701] = units_res(units);
    d.instances[0] = std::move(inst);
    store.add_object(std::move(d));
}

} // namespace

int install_sensors(ObjectStore& store, SensorHooks h) {
    add_scalar(store, 3301, "Illuminance", "urn:oma:lwm2m:ext:3301", "lx",
               std::move(h.illuminance));
    add_scalar(store, 3303, "Temperature", "urn:oma:lwm2m:ext:3303", "Cel",
               std::move(h.temperature));
    add_scalar(store, 3304, "Humidity", "urn:oma:lwm2m:ext:3304", "%RH",
               std::move(h.humidity));
    add_scalar(store, 3315, "Barometer", "urn:oma:lwm2m:ext:3315", "Pa",
               std::move(h.pressure));
    add_triaxis(store, 3313, "Accelerometer", "urn:oma:lwm2m:ext:3313", "m/s2",
                std::move(h.accel_x), std::move(h.accel_y), std::move(h.accel_z));
    add_triaxis(store, 3334, "Gyrometer", "urn:oma:lwm2m:ext:3334", "deg/s",
                std::move(h.gyro_x), std::move(h.gyro_y), std::move(h.gyro_z));
    return 0;
}

}} // namespace lwm2m::objects
