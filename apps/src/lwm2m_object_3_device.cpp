#include "lwm2m_object_3_device.hpp"

#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>

#include "lua_config.hpp"

namespace lwm2m { namespace objects {

namespace {

/// Per-RID compiled-in default values for the static metadata RIDs.
/// JSON file overrides these per RID; missing or malformed JSON file
/// falls back to these values (D6).
struct StaticDefaults {
    std::string manufacturer{"Sierra Wireless"};
    std::string modelNumber  {"LwM2M Client"};
    std::string serialNumber {"000000000000"};
    std::string firmwareVer  {"0.1"};
    std::string supportedBinding{"U"};       // RID 16
    std::string deviceType   {"LwM2M IoT Device"};
    std::string hardwareVer  {"unknown"};
    std::string softwareVer  {"0.1"};
};

/// Read `/proc/meminfo`, return the value of the named field in KB. On
/// any read / parse error returns "0" so the resource read always yields
/// a valid CoAP payload.
std::string read_meminfo(const std::string& field) {
    std::ifstream ifs("/proc/meminfo");
    if (!ifs.is_open()) return "0";
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.compare(0, field.size(), field) != 0) continue;
        // Format: "MemAvailable:    1234 kB"
        std::istringstream iss(line);
        std::string ignore_label;
        long long value = 0;
        iss >> ignore_label >> value;
        return std::to_string(value);
    }
    return "0";
}

/// Default time reader: seconds since the Unix epoch.
std::string default_time() {
    return std::to_string(static_cast<long long>(std::time(nullptr)));
}

/// Load deviceObject/0.lua overrides keyed by RID. Returns an empty
/// map on missing file / parse error (lua_config logs the error
/// itself). Per-RID type mismatches are tolerated downstream — the
/// override_or helper falls back per RID.
iot::lua_config::ResourceMap
load_overrides(const std::string& configDir) {
    std::string path = configDir;
    if (!path.empty() && path.back() != '/') path.push_back('/');
    path += "deviceObject/0.lua";
    return iot::lua_config::load_object_resources(path);
}

/// Read a per-RID override as a string. Returns the default if the
/// override is absent, excluded by include=false, or not a string.
std::string override_or(const iot::lua_config::ResourceMap& m,
                        std::uint32_t rid,
                        const std::string& def) {
    return iot::lua_config::string_or(m, rid, def);
}

/// Build a Resource value-only descriptor with a fixed read string. The
/// returned closure captures `value` by copy so later resource mutation
/// (Write) doesn't dangle.
Resource read_only_string(std::uint32_t rid,
                          const std::string& name,
                          std::string value) {
    Resource r;
    r.rid = rid; r.name = name;
    r.type = ResourceType::String;
    r.ops  = Operations::R;
    r.read = [value]() { return value; };
    return r;
}

/// A read+write string resource — the writeable state lives in a
/// shared_ptr so the read closure can observe writes.
Resource read_write_string(std::uint32_t rid,
                           const std::string& name,
                           std::string initial) {
    auto cell = std::make_shared<std::string>(std::move(initial));
    Resource r;
    r.rid = rid; r.name = name;
    r.type = ResourceType::String;
    r.ops  = Operations::RW;
    r.read  = [cell]()                  { return *cell; };
    r.write = [cell](const std::string& v) { *cell = v; return 0; };
    return r;
}

Resource live_integer(std::uint32_t rid,
                      const std::string& name,
                      std::function<std::string()> reader) {
    Resource r;
    r.rid = rid; r.name = name;
    r.type = ResourceType::Integer;
    r.ops  = Operations::R;
    r.read = std::move(reader);
    return r;
}

Resource live_time(std::uint32_t rid,
                   const std::string& name,
                   std::function<std::string()> reader) {
    Resource r;
    r.rid = rid; r.name = name;
    r.type = ResourceType::Time;
    r.ops  = Operations::R;
    r.observable = true;        // Current Time is commonly observed
    r.read = std::move(reader);
    return r;
}

Resource executable(std::uint32_t rid,
                    const std::string& name,
                    std::function<int(const std::string&)> exec) {
    Resource r;
    r.rid = rid; r.name = name;
    r.type = ResourceType::None;
    r.ops  = Operations::E;
    r.execute = std::move(exec);
    return r;
}

} // namespace

int install_device(ObjectStore& store,
                   const std::string& configDir,
                   DeviceHooks hooks) {
    StaticDefaults defs;
    auto overrides = load_overrides(configDir);

    ObjectDescriptor desc;
    desc.oid              = 3;
    desc.name             = "Device";
    desc.urn              = "urn:oma:lwm2m:oma:3:1.1";
    desc.multipleInstance = false;
    desc.mandatory        = true;

    ObjectInstance inst;
    inst.iid = 0;

    inst.resources[0]  = read_only_string(0,  "Manufacturer",
                          override_or(overrides, 0, defs.manufacturer));
    inst.resources[1]  = read_only_string(1,  "Model Number",
                          override_or(overrides, 1, defs.modelNumber));
    inst.resources[2]  = read_only_string(2,  "Serial Number",
                          override_or(overrides, 2, defs.serialNumber));
    inst.resources[3]  = read_only_string(3,  "Firmware Version",
                          override_or(overrides, 3, defs.firmwareVer));
    inst.resources[4]  = executable(4, "Reboot",
                          hooks.reboot ? std::move(hooks.reboot)
                                       : [](const std::string&) { return 0; });
    inst.resources[5]  = executable(5, "Factory Reset",
                          hooks.factoryReset ? std::move(hooks.factoryReset)
                                             : [](const std::string&) { return 0; });

    // RID 9 Battery Level — stubbed at 100 for v1. A real platform reader
    // (e.g. UPower D-Bus) is a follow-up.
    inst.resources[9]  = live_integer(9, "Battery Level",
                          []() { return std::string{"100"}; });

    inst.resources[10] = live_integer(10, "Memory Free",
                          hooks.memFreeKb ? std::move(hooks.memFreeKb)
                                          : []() { return read_meminfo("MemAvailable:"); });

    inst.resources[13] = live_time(13, "Current Time",
                          hooks.currentTime ? std::move(hooks.currentTime)
                                            : []() { return default_time(); });

    inst.resources[14] = read_write_string(14, "UTC Offset",
                          override_or(overrides, 14, std::string{"+00:00"}));
    inst.resources[15] = read_write_string(15, "Timezone",
                          override_or(overrides, 15, std::string{"UTC"}));

    inst.resources[16] = read_only_string(16, "Supported Binding and Modes",
                          override_or(overrides, 16, defs.supportedBinding));
    inst.resources[17] = read_only_string(17, "Device Type",
                          override_or(overrides, 17, defs.deviceType));
    inst.resources[18] = read_only_string(18, "Hardware Version",
                          override_or(overrides, 18, defs.hardwareVer));
    inst.resources[19] = read_only_string(19, "Software Version",
                          override_or(overrides, 19, defs.softwareVer));

    inst.resources[21] = live_integer(21, "Memory Total",
                          hooks.memTotalKb ? std::move(hooks.memTotalKb)
                                           : []() { return read_meminfo("MemTotal:"); });

    desc.instances[0] = std::move(inst);
    store.add_object(std::move(desc));
    return 0;
}

}} // namespace lwm2m::objects
