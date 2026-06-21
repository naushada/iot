#ifndef __modules_vehicle_obd_pid_hpp__
#define __modules_vehicle_obd_pid_hpp__

/// Pure OBD-II (SAE J1979) PID encode/decode for ISO 15765-4 (CAN).
///
/// No ACE, no SocketCAN, no data-store — just request framing + response
/// decoding + DTC formatting, so it is fully host-unit-testable. The CAN
/// transport (raw + ISO-TP) and ds publishing live in the iot-vehicled daemon.
///
/// Wire model (11-bit OBD-II): the tester sends a single-frame request on the
/// functional ID 0x7DF — CAN data `[0x02, mode, pid, pad…]` — and an ECU
/// answers on 0x7E8..0x7EF with `[len, mode|0x40, pid, A, B, …]`.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace vehicle {
namespace obd {

// SAE J1979 Mode 01 PIDs supported in v1 (core powertrain + fuel/intake).
enum Pid : std::uint8_t {
    kPidEngineLoad   = 0x04,
    kPidCoolantTemp  = 0x05,
    kPidRpm          = 0x0C,
    kPidSpeed        = 0x0D,
    kPidIntakeAirTmp = 0x0F,
    kPidMaf          = 0x10,
    kPidThrottle     = 0x11,
    kPidFuelLevel    = 0x2F,
};

constexpr std::uint16_t kFunctionalRequestId = 0x7DF;  // 11-bit broadcast
constexpr std::uint16_t kEcuResponseIdBase   = 0x7E8;  // 0x7E8..0x7EF
constexpr std::uint8_t  kModeCurrentData     = 0x01;   // Mode 01 (live data)
constexpr std::uint8_t  kModeDtc             = 0x03;   // Mode 03 (stored DTCs)
constexpr std::uint8_t  kPositiveReplyOffset = 0x40;   // response mode = mode|0x40
constexpr std::uint8_t  kPad                 = 0xCC;   // unused-byte padding

/// Build the 8-byte CAN-data payload for a single-PID request (default Mode 01).
/// Layout: [0x02, mode, pid, kPad, kPad, kPad, kPad, kPad].
std::array<std::uint8_t, 8> build_request(std::uint8_t pid,
                                          std::uint8_t mode = kModeCurrentData);

/// One decoded PID reading in engineering units.
struct ObdValue {
    bool         valid = false;  // false → unsupported PID or malformed frame
    std::uint8_t pid   = 0;
    double       value = 0.0;    // engineering value in `unit`
    const char*  unit  = "";     // "rpm" | "km/h" | "C" | "%" | "g/s"
};

/// Decode a Mode 01 positive response payload (the CAN data bytes
/// `[len, 0x41, pid, A, B, …]`). Returns `valid=false` on a non-0x41 service
/// id, an unsupported PID, or a frame too short for the PID's data bytes.
ObdValue decode_response(const std::uint8_t* data, std::size_t len);

/// Format an ObdValue as the decimal string published to the data-store
/// (matches the iot.sensor.* convention: trimmed, up to 2 decimals). Empty
/// when `!valid`.
std::string to_ds_string(const ObdValue& v);

/// Decode a 2-byte DTC word (Mode 03 / SAE J2012) to the standard
/// `Pxxxx`/`Cxxxx`/`Bxxxx`/`Uxxxx` string. Returns "" for an all-zero word
/// (the "no DTC" sentinel).
std::string decode_dtc(std::uint8_t hi, std::uint8_t lo);

}  // namespace obd
}  // namespace vehicle

#endif /* __modules_vehicle_obd_pid_hpp__ */
