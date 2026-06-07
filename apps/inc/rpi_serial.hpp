#ifndef __iot_rpi_serial_hpp__
#define __iot_rpi_serial_hpp__

/// PSK provisioning (task A) — hardware serial-number reader.
///
/// On a Raspberry Pi the LwM2M client uses the board serial number as
/// its endpoint and as the on-the-wire BS DTLS PSK identity. This reads
/// it from the device tree (preferred) or /proc/cpuinfo (fallback). All
/// paths are injectable so the logic is unit-testable without /proc.
///
/// See apps/docs/tdd-psk-provisioning.md.

#include <string>

namespace iot {

/// Read the hardware serial number. Tries `devtree_path` first
/// (/proc/device-tree/serial-number — a NUL-terminated string), then
/// parses the `Serial` line from `cpuinfo_path`. Returns "" when neither
/// yields a value (the caller decides the fallback; we do NOT invent a
/// magic all-zero serial).
std::string read_rpi_serial(
    const std::string& devtree_path = "/proc/device-tree/serial-number",
    const std::string& cpuinfo_path = "/proc/cpuinfo");

/// True when running on a Raspberry Pi — `model_path`
/// (/proc/device-tree/model) contains "Raspberry Pi". Used to decide
/// whether to auto-fill the serial at startup (RPi) or leave it for the
/// installer to enter via device-ui (non-RPi).
bool is_rpi(const std::string& model_path = "/proc/device-tree/model");

} // namespace iot

#endif /* __iot_rpi_serial_hpp__ */
