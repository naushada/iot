#include "lwm2m_object_stubs.hpp"

#include <cstdio>
#include <variant>

#include "lua_config.hpp"
#include "lwm2m_object_cert.hpp"
#include "lwm2m_object_firmware.hpp"

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

int install_connmon(ObjectStore& store, std::function<std::string()> ipReader) {
    ObjectDescriptor d;
    d.oid = 4; d.name = "Connectivity Monitoring";
    d.urn = "urn:oma:lwm2m:oma:4:1.1";
    d.mandatory = false; d.multipleInstance = false;

    auto inst = instance_with(0);
    inst.resources[0] = read_only(0,  "Network Bearer",          ResourceType::Integer, "41");   // 41 = Ethernet
    inst.resources[1] = read_only(1,  "Available Network Bearer",ResourceType::Integer, "41");
    inst.resources[2] = read_only(2,  "Radio Signal Strength",   ResourceType::Integer, "-70");
    inst.resources[3] = read_only(3,  "Link Quality",            ResourceType::Integer, "100");
    if (ipReader) {
        // Live IP (wifi.dhcp.ip from ds) so a server Read /4/0/4 surfaces the
        // device's LAN IP; the cloud shows it in the Endpoints table.
        Resource ip; ip.rid = 4; ip.name = "IP Addresses";
        ip.type = ResourceType::String; ip.ops = Operations::R;
        ip.read = std::move(ipReader);
        inst.resources[4] = std::move(ip);
    } else {
        inst.resources[4] = read_only(4, "IP Addresses", ResourceType::String, "0.0.0.0");
    }
    inst.resources[6] = read_only(6,  "Link Utilization",        ResourceType::Integer, "0");
    inst.resources[7] = read_only(7,  "APN",                     ResourceType::String,  "");
    d.instances[0] = std::move(inst);
    store.add_object(std::move(d));
    return 0;
}

namespace {
/// Observable, read-only resource backed by `rd` (live) or a static default.
Resource live_or_static(std::uint32_t rid, const std::string& name,
                        ResourceType type, std::function<std::string()> rd,
                        std::string dflt) {
    Resource r;
    r.rid = rid; r.name = name; r.type = type; r.ops = Operations::R;
    r.observable = true;
    if (rd) r.read = std::move(rd);
    else    r.read = [dflt]() { return dflt; };
    return r;
}
} // namespace

int install_location(ObjectStore& store, LocationHooks h) {
    ObjectDescriptor d;
    d.oid = 6; d.name = "Location";
    d.urn = "urn:oma:lwm2m:oma:6:1.0";
    d.mandatory = false; d.multipleInstance = false;

    auto inst = instance_with(0);
    // Bound to gps.* (cellular-client daemon) in the client build; unset → "0".
    inst.resources[0] = live_or_static(0, "Latitude",  ResourceType::Float, std::move(h.latitude),  "0.0");
    inst.resources[1] = live_or_static(1, "Longitude", ResourceType::Float, std::move(h.longitude), "0.0");
    inst.resources[2] = live_or_static(2, "Altitude",  ResourceType::Float, std::move(h.altitude),  "0.0");
    inst.resources[5] = live_or_static(5, "Timestamp", ResourceType::Time,  std::move(h.timestamp), "0");
    inst.resources[6] = live_or_static(6, "Speed",     ResourceType::Float, std::move(h.speed),     "0.0");
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

int install_vehicle(ObjectStore& store, VehicleHooks h) {
    ObjectDescriptor d;
    d.oid = 33000; d.name = "Vehicle Telemetry";
    d.urn = "urn:oma:lwm2m:x:33000";          // private-use range (no OMA reg)
    d.mandatory = false; d.multipleInstance = false;

    auto inst = instance_with(0);
    // Bound to vehicle.* (iot-vehicled / OBD-II) in the client build; unset →
    // static default. A server Read/Observe /33000/0/* surfaces live vehicle
    // telemetry to the cloud (map + dashboards). RID 8 (DTC) is fed once the
    // ISO-TP Mode 03 path lands; RID 9 (MIL) is reserved.
    inst.resources[0]  = live_or_static(0,  "Speed",           ResourceType::Float,  std::move(h.speed),    "0");
    inst.resources[1]  = live_or_static(1,  "RPM",             ResourceType::Float,  std::move(h.rpm),      "0");
    inst.resources[2]  = live_or_static(2,  "Coolant",         ResourceType::Float,  std::move(h.coolant),  "0");
    inst.resources[3]  = live_or_static(3,  "Throttle",        ResourceType::Float,  std::move(h.throttle), "0");
    inst.resources[4]  = live_or_static(4,  "Engine Load",     ResourceType::Float,  std::move(h.load),     "0");
    inst.resources[5]  = live_or_static(5,  "Fuel Level",      ResourceType::Float,  std::move(h.fuel),     "0");
    inst.resources[6]  = live_or_static(6,  "Intake Air Temp", ResourceType::Float,  std::move(h.iat),      "0");
    inst.resources[7]  = live_or_static(7,  "MAF",             ResourceType::Float,  std::move(h.maf),      "0");
    inst.resources[8]  = live_or_static(8,  "DTC List",        ResourceType::String, std::move(h.dtc),      "");
    inst.resources[10] = live_or_static(10, "Link",            ResourceType::String, std::move(h.link),     "down");
    d.instances[0] = std::move(inst);
    store.add_object(std::move(d));
    return 0;
}

// The Security (OID 0) and Server (OID 1) objects are NOT seeded from static
// config — they are delivered entirely by the Bootstrap server at /bs and
// created in the live store by the Bootstrap client's apply_commit (which
// add_object()s OID 0 / OID 1 on demand). So there are no install_security /
// install_server stubs: provisioning is 100% data-store driven.

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
    // OID 5 with W/E wired (read-only when no apply hooks are supplied —
    // server / Discover use). See lwm2m_object_firmware.cpp.
    return install_firmware_apply(store, configDir, {});
}

int install_canonical_objects(ObjectStore& store,
                              const std::string& configDir,
                              DeviceHooks deviceHooks,
                              FwHooks fwHooks,
                              CertHooks certHooks,
                              LocationHooks locationHooks) {
    int rc = 0;
    rc |= install_access_control(store, configDir);
    // Lift the Object-4 IP reader out before deviceHooks is moved into the
    // Device-object installer (it backs /4/0/4, wired here for convenience).
    auto ipReader = std::move(deviceHooks.ipAddresses);
    rc |= install_device(store, configDir, std::move(deviceHooks));
    rc |= install_connmon(store, std::move(ipReader));
    rc |= install_firmware_apply(store, configDir, std::move(fwHooks));
    rc |= install_location(store, std::move(locationHooks));
    rc |= install_connstats(store);
    // Custom OID 2048 — cloud-pushed VPN/TLS credential family. Default
    // store writes the cert family under /etc/iot/vpn (= vpn.{ca,cert,key}.path
    // defaults); a cert sidecar reloads openvpn-client on the change.
    rc |= install_cert(store, "/etc/iot/vpn", std::move(certHooks));
    return rc;
}

}} // namespace lwm2m::objects
