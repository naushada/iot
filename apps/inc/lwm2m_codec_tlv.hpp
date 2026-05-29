#ifndef __lwm2m_codec_tlv_hpp__
#define __lwm2m_codec_tlv_hpp__

#include <cstdint>
#include <string>
#include <vector>

/**
 * @file lwm2m_codec_tlv.hpp
 * @brief OMA LwM2M TLV codec (Core spec Annex C).
 *
 * Carved out of lwm2m_adapter.{hpp,cpp} in L1. Implements:
 *  - REQ-ENC-001: round-trip of String, Integer, Float, Boolean, Opaque,
 *    Time, ObjLink.
 *  - REQ-ENC-002: malformed input is rejected with a non-zero return code
 *    rather than aborting the process.
 *  - REQ-ENC-008: codec is consumed via the lwm2m_codec_registry interface
 *    (registered for content-format 11542 = application/vnd.oma.lwm2m+tlv).
 *
 * The in-memory shape (LwM2MObject / LwM2MObjectData) is intentionally
 * preserved from the original implementation so existing callers in
 * coap_adapter.cpp / udp_adapter.cpp keep compiling unchanged.
 */

/* ────────── TLV type-field enumerations ────────── */

enum TypeFieldOfTLV_t : std::uint8_t {
    TypeBits76_ObjectInstance_OneOrMoreResourceTLV_00      = 0,
    TypeBits76_ResourceInstance_OneOrMultipleResourceTLV_01 = 1,
    TypeBits76_MultipleResource_OneOrMoreResourceInstanceTLV_10 = 2,
    TypeBits76_ResourceWithValue_11                        = 3,
};

enum LengthOfTheIdentifier_t : std::uint8_t {
    TypeBit5_LengthOfTheIdentifier8BitsLong_0  = 0,
    TypeBit5_LengthOfTheIdentifier16BitsLong_1 = 1,
};

enum LengthOfTheType_t : std::uint8_t {
    TypeBits43_NoTypeLengthField_00     = 0,
    TypeBits43_8BitsTypeLengthField_01  = 1,
    TypeBits43_16BitsTypeLengthField_10 = 2,
    TypeBits43_24BitsTypeLengthField_11 = 3,
};

/* ────────── In-memory shape (preserved from lwm2m_adapter.hpp) ────────── */

struct LwM2MObjectData {
    std::uint32_t              m_oiid{0};
    std::uint32_t              m_rid{0};
    std::uint32_t              m_riid{0};
    std::uint32_t              m_ridlength{0};
    std::vector<std::uint8_t>  m_ridvalue;

    LwM2MObjectData() = default;
    LwM2MObjectData& clear() {
        m_oiid = 0;
        m_riid = 0;
        m_ridlength = 0;
        m_ridvalue.clear();
        return *this;
    }
};

struct LwM2MObject {
    std::uint32_t                  m_oid{0};
    std::vector<LwM2MObjectData>   m_value;

    LwM2MObject() = default;
    LwM2MObject& clear() {
        m_oid = 0;
        m_value.clear();
        return *this;
    }
};

/* ────────── Codec API ────────── */

namespace lwm2m { namespace tlv {

/// Result code for decode_container.
///   0  = success, possibly partial
///  -1  = malformed input (truncated header, length overflow, unknown type)
///         the input is fully consumed and decoded records up to that point
///         are written to `out`. The caller should drop the message and
///         reply 4.00 Bad Request (REQ-ENC-002).
int decode_container(const std::string& payload,
                     LwM2MObjectData& scratch,
                     LwM2MObject& out);

/// Encode an opaque / string value as one TLV record. Returns 0.
int encode_string(TypeFieldOfTLV_t kind,
                  const std::string& value,
                  std::uint16_t id,
                  std::string& out);

/// Encode an unsigned integer as one TLV record. Width chosen by magnitude
/// (1/2/4 bytes). Returns 0. Matches the original two's-complement / network
/// byte order rules from lwm2m_adapter.cpp.
int encode_uint(TypeFieldOfTLV_t kind,
                std::uint32_t value,
                std::uint16_t id,
                std::string& out);

/// Encode a signed 64-bit Time value as one TLV record (Annex C). Big-endian,
/// width 1/2/4/8 by magnitude. Returns 0. NEW in L1 — closes a gap in
/// REQ-ENC-001 (Time type was missing).
int encode_time(TypeFieldOfTLV_t kind,
                std::int64_t value,
                std::uint16_t id,
                std::string& out);

/// Encode a boolean as one TLV record (single byte 0x00 / 0x01). Returns 0.
int encode_bool(TypeFieldOfTLV_t kind,
                bool value,
                std::uint16_t id,
                std::string& out);

/// Encode an ObjLink (oid:iid pair) as a 4-byte big-endian record per
/// Annex C. NEW in L1 — closes a gap in REQ-ENC-001.
int encode_objlink(TypeFieldOfTLV_t kind,
                   std::uint16_t targetOid,
                   std::uint16_t targetIid,
                   std::uint16_t id,
                   std::string& out);

}} // namespace lwm2m::tlv

#endif /*__lwm2m_codec_tlv_hpp__*/
