#ifndef __lwm2m_object_firmware_hpp__
#define __lwm2m_object_firmware_hpp__

#include <cstdint>
#include <functional>
#include <string>

#include "lwm2m_object_store.hpp"

/**
 * @file lwm2m_object_firmware.hpp
 * @brief LwM2M Firmware Update Object (OID 5) — .ipk OTA, pull delivery.
 *
 * Phase-1 OTA: the cloud (lwm2m-dm) WRITEs the package URI to /5/0/1 then
 * EXECUTEs /5/0/2; the device pulls the .ipk from the cloud firmware feed
 * (over the VPN tunnel) and applies it via opkg. The apply is launched as a
 * DETACHED process (systemd transient unit) so opkg-replacing the running
 * `lwm2m` binary + restarting the unit can't kill the in-flight install.
 *
 *   RID 1  Package URI   String   W   the .ipk URL (may carry ?sha256=<hex>)
 *   RID 2  Update        ----     E   launch the detached apply
 *   RID 3  State         Integer  R   0 Idle,1 Downloading,2 Downloaded,3 Updating
 *   RID 5  Update Result Integer  R   0 Initial,1 Success,5 Integrity,8 URI,9 Install
 *   RID 7  PkgVersion    String   R   installed package version
 *   RID 6/8/9            ...      R   PkgName / Protocol / Delivery (from lua)
 *   RID 26 Update Reason String   R   VENDOR extension: human cause for a
 *                                     terminal Result (iot.update.reason, e.g.
 *                                     "no .ipk in bundle") — polled by our own
 *                                     lwm2m-dm so the cloud UI can say WHY a
 *                                     campaign failed. Outside OMA Object 5
 *                                     (RIDs 0-13); a foreign server ignores it.
 *
 * State/Result/Version are read live from the data store (the detached
 * updater writes iot.update.state/result/version), so a server READ after the
 * client restarts reflects the real outcome. The object stays pure: the
 * client wiring injects ds-backed read hooks + the launcher; tests inject fakes.
 */

namespace lwm2m { namespace objects {

constexpr std::uint32_t kFirmwareObjectOid = 5;
/// Vendor resource: human-readable cause for a terminal /5/0/5 result.
constexpr std::uint32_t kFirmwareRidReason = 26;

struct FwHooks {
    /// Launch the detached apply for `uri`. Return 0 on launch (→ 2.04
    /// Changed on the EXECUTE), non-zero to report 5.00. Default:
    /// `systemd-run --unit=iot-ota-apply --collect /usr/bin/iot-ota-apply <uri>`.
    std::function<int(const std::string& uri)> launch;

    /// Live readbacks (so a server READ reflects the detached updater's
    /// progress). Default (unset) → 0/0/"" from the in-memory fallback.
    std::function<long long()>   read_state;     ///< /5/0/3 (iot.update.state)
    std::function<long long()>   read_result;    ///< /5/0/5 (iot.update.result)
    std::function<std::string()> read_version;   ///< /5/0/7 (iot.update.version)
    std::function<std::string()> read_reason;    ///< /5/0/26 (iot.update.reason)
};

/// Install OID 5/0 with RID 1 (W) + RID 2 (E) wired and RID 3/5/7 live.
/// `configDir` supplies RID 6/8/9 defaults from firmwareObject/0.lua.
/// With default (empty) hooks the object is effectively read-only (server /
/// Discover use). Returns 0 on success.
int install_firmware_apply(ObjectStore& store,
                           const std::string& configDir,
                           FwHooks hooks = {});

}} // namespace lwm2m::objects

#endif /*__lwm2m_object_firmware_hpp__*/
