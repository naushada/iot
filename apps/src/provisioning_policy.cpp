#include "provisioning_policy.hpp"

namespace iot {

EndpointResolution resolve_endpoint(
    const std::string&                cli_ep,
    const std::optional<std::string>& ds_serial,
    bool                              is_rpi,
    const std::string&                detected_serial) {

    // 1) Explicit CLI override always wins; never auto-writes a serial.
    if (!cli_ep.empty())
        return {cli_ep, /*serial_to_write*/"", /*ready*/true};

    // 2) A serial already in the data-store (installer-entered on non-RPi,
    //    or a prior RPi auto-fill) is authoritative.
    if (ds_serial && !ds_serial->empty())
        return {*ds_serial, "", true};

    // 3) RPi with a freshly detected serial → use it and signal that the
    //    caller should persist it to iot.serial (auto-fill).
    if (is_rpi && !detected_serial.empty())
        return {detected_serial, detected_serial, true};

    // 4) Non-RPi with nothing provisioned yet → defer registration.
    return {"", "", false};
}

bool should_restart_on_psk_change(bool               initialized,
                                  const std::string& loaded,
                                  const std::string& observed) {
    return initialized && observed != loaded;
}

bool is_coap_client_error(std::uint8_t coap_code) {
    return (coap_code >> 5) == 4;
}

bool should_rebootstrap(bool dm_dtls_failed, bool dm_registration_rejected) {
    return dm_dtls_failed || dm_registration_rejected;
}

} // namespace iot
