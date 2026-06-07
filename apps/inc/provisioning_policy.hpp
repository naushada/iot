#ifndef __iot_provisioning_policy_hpp__
#define __iot_provisioning_policy_hpp__

/// PSK provisioning (tasks E/K/G) — pure decision logic.
///
/// The client wiring in main.cpp is integration-heavy (reactor, DTLS,
/// data-store). To keep the *correctness-critical* decisions testable we
/// factor them out here as pure functions with no I/O, and let main.cpp
/// be thin glue around them.
///
/// See apps/docs/tdd-psk-provisioning.md.

#include <cstdint>
#include <optional>
#include <string>

namespace iot {

/// Task E — resolve the LwM2M endpoint / on-the-wire BS PSK identity
/// from the available sources, in priority order.
struct EndpointResolution {
    std::string endpoint;         ///< effective endpoint ("" → defer registration)
    std::string serial_to_write;  ///< non-empty → client should persist iot.serial (RPi auto-fill)
    bool        ready = false;    ///< endpoint usable now
};

/// Priority: explicit CLI `ep=` > data-store `iot.serial` > RPi
/// auto-detected serial. On non-RPi hardware with no serial yet, returns
/// {ready=false} so the caller defers registration until the installer
/// enters it via device-ui.
EndpointResolution resolve_endpoint(
    const std::string&                cli_ep,
    const std::optional<std::string>& ds_serial,
    bool                              is_rpi,
    const std::string&                detected_serial);

/// Task G — should the client self-exit (→ systemd relaunch) because a
/// watched PSK key changed underneath it? The caller tracks the value it
/// has loaded; self-writes update `loaded` first, so they don't trip
/// this. Only a genuine post-init change (e.g. an engineer editing the
/// PSK via ds-cli in dev-mode) returns true.
bool should_restart_on_psk_change(bool               initialized,
                                  const std::string& loaded,
                                  const std::string& observed);

/// True when `coap_code` is a 4.xx client-error class (upper 3 bits == 4).
/// CoAP encodes the response code as (class<<5)|detail, so 4.03 == 0x83.
bool is_coap_client_error(std::uint8_t coap_code);

/// Task K — should the client fall back to the bootstrap server for fresh
/// DM credentials? Triggers on a DM DTLS auth failure OR an LwM2M
/// registration rejection (4.0x). Cooldown/backoff is the caller's job.
bool should_rebootstrap(bool dm_dtls_failed, bool dm_registration_rejected);

} // namespace iot

#endif /* __iot_provisioning_policy_hpp__ */
