#ifndef __lwm2m_codec_senml_hpp__
#define __lwm2m_codec_senml_hpp__

#include <cstdint>
#include <string>
#include <vector>

/**
 * @file lwm2m_codec_senml.hpp
 * @brief SenML JSON (CF=110) and SenML CBOR (CF=112) codecs.
 *
 * Closes REQ-ENC-003 and REQ-ENC-004 per RFC 8428 §5 / §6.
 *
 * Scope: the LwM2M-relevant subset of SenML — Base Name (`bn`), Name
 * (`n`), Base Time (`bt`), Time (`t`), Numeric Value (`v`), String
 * Value (`vs`), Boolean Value (`vb`), Data Value (`vd`). `bt`/`t` carry
 * per-record timestamps for batched telemetry (LwM2M Send backfill).
 * Unix-Time (`ut`), Unit (`u`, `bu`), Sum (`s`, `bs`), and Value-Sum
 * aggregation remain out of v1 scope — Core spec doesn't require them on
 * a 1.1 device.
 *
 * The codec produces / consumes a flat `std::vector<Record>`. Base
 * Name accumulation (RFC 8428 §4.5: subsequent records inherit the
 * first record's `bn`) is materialised on decode, so every output
 * record has its full path resolved into `name`; `baseName` on output
 * carries the original `bn` for symmetric round-trip on encode.
 *
 * The DM layer wraps this module via `lwm2m::DmClient` when an
 * incoming request declares CF 110 or 112. The codec registry holds
 * thin `ICodec` wrappers (lwm2m_codec_registry.cpp) so other code
 * can also discover them by CF code.
 */

namespace lwm2m { namespace senml {

/// SenML value shape. RFC 8428 §4.3 lets exactly one value field be
/// present per record. We also support `None` for the case where the
/// caller is constructing a record incrementally (encoder rejects it).
enum class ValueKind : std::uint8_t {
    None,
    Numeric,
    String,
    Boolean,
    Data,
};

struct Record {
    /// Base name carried by the first record only on the wire; we keep
    /// it set on every output record so resolving full paths is a
    /// concatenation, not a state machine.
    std::string baseName;
    /// Relative name appended to `baseName` to form the full URI.
    std::string name;

    /// The full path `baseName + name`. Convenience for callers.
    std::string path() const { return baseName + name; }

    ValueKind kind{ValueKind::None};
    /// Numeric value. `isFloat == true` → emit as JSON number with a
    /// fractional part; `false` → integer. CBOR encodes accordingly.
    double      numericValue{0.0};
    bool        isFloat{false};
    std::string stringValue;
    bool        booleanValue{false};
    /// Raw binary for `vd`. The codec handles base64url encode/decode
    /// for SenML JSON; SenML CBOR carries it as a byte-string directly.
    std::string dataValue;

    /// Base Time (`bt`, RFC 8428 §4.5.2). Like `bn`, it is carried by the
    /// first record on the wire and kept on every output record so resolving
    /// a record's absolute time is an addition, not a state machine. A
    /// record's effective time is `baseTime + time`. Needed for timestamped
    /// telemetry batches (LwM2M Send backfill); absent on plain reads.
    double baseTime{0.0};
    bool   hasBaseTime{false};
    /// Per-record Time (`t`), a relative offset added to `baseTime` (or an
    /// absolute time when no `bt` is present, per §4.5.3). Absent → hasTime
    /// false (a 0 offset).
    double time{0.0};
    bool   hasTime{false};

    /// Effective absolute time = baseTime + time. Only meaningful when
    /// hasBaseTime || hasTime.
    double effectiveTime() const { return baseTime + time; }
};

/* ────────── JSON (RFC 8428 §5) ────────── */

/// Encode a record list as SenML JSON. The first record's `baseName`
/// becomes `bn` on the wire; all subsequent records' `baseName`s must
/// match (else returns empty string).
std::string encode_json(const std::vector<Record>& records);

/// Decode SenML JSON. Returns 0 on success, -1 on malformed input
/// (parse error, missing name, multiple value fields, unknown label).
int decode_json(const std::string& bytes, std::vector<Record>& out);

/* ────────── CBOR (RFC 8428 §6) ────────── */

/// Encode a record list as SenML CBOR (integer-keyed maps).
std::string encode_cbor(const std::vector<Record>& records);

/// Decode SenML CBOR. Returns 0 on success, -1 on malformed input.
int decode_cbor(const std::string& bytes, std::vector<Record>& out);

}} // namespace lwm2m::senml

#endif /*__lwm2m_codec_senml_hpp__*/
