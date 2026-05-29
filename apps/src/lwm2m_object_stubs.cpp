#include "lwm2m_object_stubs.hpp"

namespace lwm2m { namespace objects {

namespace {

Resource read_only(std::uint32_t rid,
                   const std::string& name,
                   ResourceType type,
                   std::string value) {
    Resource r;
    r.rid = rid; r.name = name; r.type = type; r.ops = Operations::R;
    r.read = [value]() { return value; };
    return r;
}

ObjectInstance instance_with(std::uint32_t iid) {
    ObjectInstance i; i.iid = iid; return i;
}

} // namespace

int install_connmon(ObjectStore& store) {
    ObjectDescriptor d;
    d.oid = 4; d.name = "Connectivity Monitoring";
    d.urn = "urn:oma:lwm2m:oma:4:1.1";
    d.mandatory = false; d.multipleInstance = false;

    auto inst = instance_with(0);
    inst.resources[0] = read_only(0,  "Network Bearer",          ResourceType::Integer, "41");   // 41 = Ethernet
    inst.resources[1] = read_only(1,  "Available Network Bearer",ResourceType::Integer, "41");
    inst.resources[2] = read_only(2,  "Radio Signal Strength",   ResourceType::Integer, "-70");
    inst.resources[3] = read_only(3,  "Link Quality",            ResourceType::Integer, "100");
    inst.resources[4] = read_only(4,  "IP Addresses",            ResourceType::String,  "0.0.0.0");
    inst.resources[6] = read_only(6,  "Link Utilization",        ResourceType::Integer, "0");
    inst.resources[7] = read_only(7,  "APN",                     ResourceType::String,  "");
    d.instances[0] = std::move(inst);
    store.add_object(std::move(d));
    return 0;
}

int install_location(ObjectStore& store) {
    ObjectDescriptor d;
    d.oid = 6; d.name = "Location";
    d.urn = "urn:oma:lwm2m:oma:6:1.0";
    d.mandatory = false; d.multipleInstance = false;

    auto inst = instance_with(0);
    inst.resources[0] = read_only(0, "Latitude",  ResourceType::Float, "0.0");
    inst.resources[1] = read_only(1, "Longitude", ResourceType::Float, "0.0");
    inst.resources[5] = read_only(5, "Timestamp", ResourceType::Time,  "0");
    d.instances[0] = std::move(inst);
    store.add_object(std::move(d));
    return 0;
}

int install_connstats(ObjectStore& store) {
    ObjectDescriptor d;
    d.oid = 7; d.name = "Connectivity Statistics";
    d.urn = "urn:oma:lwm2m:oma:7:1.0";
    d.mandatory = false; d.multipleInstance = false;

    auto inst = instance_with(0);
    inst.resources[0] = read_only(0, "SMS Tx Counter",  ResourceType::Integer, "0");
    inst.resources[1] = read_only(1, "SMS Rx Counter",  ResourceType::Integer, "0");
    inst.resources[2] = read_only(2, "Tx Data",         ResourceType::Integer, "0");
    inst.resources[3] = read_only(3, "Rx Data",         ResourceType::Integer, "0");
    d.instances[0] = std::move(inst);
    store.add_object(std::move(d));
    return 0;
}

int install_canonical_objects(ObjectStore& store,
                              const std::string& configDir,
                              DeviceHooks deviceHooks) {
    int rc = 0;
    rc |= install_device(store, configDir, std::move(deviceHooks));
    rc |= install_connmon(store);
    rc |= install_location(store);
    rc |= install_connstats(store);
    return rc;
}

}} // namespace lwm2m::objects
