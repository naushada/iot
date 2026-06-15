#ifndef __lwm2m_object_3_device_hpp__
#define __lwm2m_object_3_device_hpp__

#include <functional>
#include <memory>
#include <string>

#include "lwm2m_object_store.hpp"

/**
 * @file lwm2m_object_3_device.hpp
 * @brief LwM2M Device Object (OID 3) installer.
 *
 * L8 deliverable per RDD REQ-OBJ-003 / design §3 / decision D6.
 *
 * `install_device` registers OID 3 instance 0 in the given ObjectStore
 * with the canonical resources from OMA-SpecWorks. Resource backing:
 *
 *   - Static metadata RIDs (0 Manufacturer, 1 Model, 2 Serial, 3 Firmware
 *     Version, 16 Supported Binding, 17 Device Type, 18 Hardware
 *     Version, 19 Software Version) come from
 *     `apps/config/deviceObject/0.lua` with compiled-in fallbacks
 *     applied per-RID.
 *   - Live RIDs (10 Memory Free, 13 Current Time, 21 Memory Total)
 *     bind to Linux platform readers regardless of Lua content.
 *   - Executable RIDs (4 Reboot, 5 Factory Reset) install no-op
 *     callbacks by default. Callers wire in real platform hooks via
 *     the `DeviceHooks` struct below.
 */

namespace lwm2m { namespace objects {

/// Platform-specific overrides for the executable + live RIDs. All
/// fields are optional; an unset hook keeps the default behaviour
/// (no-op for executables, /proc/meminfo + time() for live values).
struct DeviceHooks {
    /// Called when the server invokes POST /3/0/4 (Reboot). Returns 0
    /// on success; CoAP code on failure.
    std::function<int(const std::string& args)> reboot;
    /// Called when the server invokes POST /3/0/5 (Factory Reset).
    std::function<int(const std::string& args)> factoryReset;
    /// Optional override for memory-free; production should leave nullptr
    /// so /proc/meminfo is used. Tests inject a fake here.
    std::function<std::string()> memFreeKb;
    /// Same for memory-total.
    std::function<std::string()> memTotalKb;
    /// Same for current time (epoch seconds as decimal string).
    std::function<std::string()> currentTime;
    /// Optional reader for RID 3 (Firmware Version). When set, /3/0/3
    /// returns its result live (e.g. iot.version from ds) instead of the
    /// deviceObject/0.lua value or the compiled-in fallback. Leave nullptr
    /// to keep the static metadata behaviour.
    std::function<std::string()> firmwareVersion;
};

/// Install OID 3 into `store`. `configDir` is the path under which the
/// installer looks for `deviceObject/0.lua`; pass `apps/config/` for
/// the in-repo run or wherever the operator stages config.
/// Returns 0 on success, -1 if the store rejected the registration.
int install_device(ObjectStore& store,
                   const std::string& configDir,
                   DeviceHooks hooks = {});

}} // namespace lwm2m::objects

#endif /*__lwm2m_object_3_device_hpp__*/
