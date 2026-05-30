#include "lwm2m_object_stubs.hpp"

#include <cstdio>
#include <variant>

#include "lua_config.hpp"

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

/// Stringify a lua_config value into its CoAP text/plain wire form.
/// Used by the Lua-backed installers below to fill a `read_only`
/// resource. Empty string for unset / opaque values (caller picks
/// the type via a separate guess).
std::string value_to_text(const iot::lua_config::ResourceValue& v) {
    if (auto* b = std::get_if<bool>(&v))         return *b ? "1" : "0";
    if (auto* n = std::get_if<long long>(&v))    return std::to_string(*n);
    if (auto* d = std::get_if<double>(&v)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", *d);
        return buf;
    }
    if (auto* s = std::get_if<std::string>(&v))  return *s;
    // Opaque bytes / monostate → empty (server returns no value).
    return {};
}

/// Pick a ResourceType from the value's variant alternative so the
/// installed Resource declares a sensible type for SenML/TLV encoders.
ResourceType value_to_type(const iot::lua_config::ResourceValue& v) {
    if (std::get_if<bool>(&v))                                return ResourceType::Boolean;
    if (std::get_if<long long>(&v))                           return ResourceType::Integer;
    if (std::get_if<double>(&v))                              return ResourceType::Float;
    if (std::get_if<std::string>(&v))                         return ResourceType::String;
    if (std::get_if<iot::lua_config::OpaqueBytes>(&v))        return ResourceType::Opaque;
    return ResourceType::String;
}

/// Generic "load one Lua file → one ObjectInstance worth of read-only
/// resources" helper. Empty Lua file (or one whose loader returns an
/// empty map) yields an empty instance so the Object still advertises.
ObjectInstance instance_from_lua(std::uint32_t iid, const std::string& path) {
    auto m = iot::lua_config::load_object_resources(path);
    ObjectInstance inst = instance_with(iid);
    for (const auto& [rid, rec] : m) {
        if (!rec.include) continue;
        inst.resources[rid] =
            read_only(rid, rec.description,
                      value_to_type(rec.value),
                      value_to_text(rec.value));
    }
    return inst;
}

std::string join_path(const std::string& configDir, const std::string& tail) {
    std::string p = configDir;
    if (!p.empty() && p.back() != '/') p.push_back('/');
    return p + tail;
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

int install_security(ObjectStore& store, const std::string& configDir) {
    ObjectDescriptor d;
    d.oid = 0; d.name = "LwM2M Security";
    d.urn = "urn:oma:lwm2m:oma:0:1.1";
    d.mandatory = true; d.multipleInstance = true;

    // Mirror every securityObject/<iid>.lua under configDir. The two
    // canonical files are 0.lua (bootstrap account) and 1.lua (DM).
    for (std::uint16_t iid : {0, 1}) {
        auto inst = instance_from_lua(
            iid, join_path(configDir, "securityObject/" +
                                       std::to_string(iid) + ".lua"));
        if (!inst.resources.empty()) {
            d.instances[iid] = std::move(inst);
        }
    }
    if (d.instances.empty()) return 0;     // no file → don't advertise
    store.add_object(std::move(d));
    return 0;
}

int install_server(ObjectStore& store, const std::string& configDir) {
    ObjectDescriptor d;
    d.oid = 1; d.name = "LwM2M Server";
    d.urn = "urn:oma:lwm2m:oma:1:1.1";
    d.mandatory = true; d.multipleInstance = true;

    for (std::uint16_t iid : {0, 1}) {
        auto inst = instance_from_lua(
            iid, join_path(configDir, "serverObject/" +
                                       std::to_string(iid) + ".lua"));
        if (!inst.resources.empty()) {
            d.instances[iid] = std::move(inst);
        }
    }
    if (d.instances.empty()) return 0;
    store.add_object(std::move(d));
    return 0;
}

int install_access_control(ObjectStore& store, const std::string& configDir) {
    auto inst = instance_from_lua(
        0, join_path(configDir, "accessControlObject/0.lua"));
    if (inst.resources.empty()) return 0;

    ObjectDescriptor d;
    d.oid = 2; d.name = "Access Control";
    d.urn = "urn:oma:lwm2m:oma:2:1.0";
    d.mandatory = false; d.multipleInstance = true;
    d.instances[0] = std::move(inst);
    store.add_object(std::move(d));
    return 0;
}

int install_firmware(ObjectStore& store, const std::string& configDir) {
    auto inst = instance_from_lua(
        0, join_path(configDir, "firmwareObject/0.lua"));
    if (inst.resources.empty()) return 0;

    ObjectDescriptor d;
    d.oid = 5; d.name = "Firmware Update";
    d.urn = "urn:oma:lwm2m:oma:5:1.0";
    d.mandatory = false; d.multipleInstance = false;
    d.instances[0] = std::move(inst);
    store.add_object(std::move(d));
    return 0;
}

int install_canonical_objects(ObjectStore& store,
                              const std::string& configDir,
                              DeviceHooks deviceHooks) {
    int rc = 0;
    rc |= install_security(store, configDir);
    rc |= install_server(store, configDir);
    rc |= install_access_control(store, configDir);
    rc |= install_device(store, configDir, std::move(deviceHooks));
    rc |= install_connmon(store);
    rc |= install_firmware(store, configDir);
    rc |= install_location(store);
    rc |= install_connstats(store);
    return rc;
}

}} // namespace lwm2m::objects
