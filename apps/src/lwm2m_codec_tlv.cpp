#include "lwm2m_codec_tlv.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <sstream>

namespace lwm2m { namespace tlv {

/* ───────────────────────── decode ────────────────────────── */

namespace {

/// Bounded cursor over a byte buffer. Replaces the old istringstream-based
/// recursive parser. read_*() return false on under-run and leave the
/// cursor where it was; the caller fails the whole record.
class Cursor {
public:
    Cursor(const std::uint8_t* p, std::size_t n) : m_p(p), m_n(n), m_i(0) {}

    bool read_u8(std::uint8_t& out) {
        if (m_i + 1 > m_n) return false;
        out = m_p[m_i++];
        return true;
    }

    bool read_u16(std::uint16_t& out) {
        if (m_i + 2 > m_n) return false;
        out = static_cast<std::uint16_t>(m_p[m_i]) << 8 |
              static_cast<std::uint16_t>(m_p[m_i + 1]);
        m_i += 2;
        return true;
    }

    bool read_u24(std::uint32_t& out) {
        if (m_i + 3 > m_n) return false;
        out =  static_cast<std::uint32_t>(m_p[m_i])     << 16 |
               static_cast<std::uint32_t>(m_p[m_i + 1]) <<  8 |
               static_cast<std::uint32_t>(m_p[m_i + 2]);
        m_i += 3;
        return true;
    }

    bool read_bytes(std::vector<std::uint8_t>& out, std::size_t len) {
        if (m_i + len > m_n) return false;
        out.assign(m_p + m_i, m_p + m_i + len);
        m_i += len;
        return true;
    }

    bool eof() const { return m_i >= m_n; }
    std::size_t remaining() const { return m_n - m_i; }
    std::size_t pos() const { return m_i; }

private:
    const std::uint8_t* m_p;
    std::size_t         m_n;
    std::size_t         m_i;
};

/// Read the identifier (8 or 16 bit), choosing width by the type's bit-5.
bool read_identifier(Cursor& c, LengthOfTheIdentifier_t bit5, std::uint32_t& out) {
    if (bit5 == TypeBit5_LengthOfTheIdentifier8BitsLong_0) {
        std::uint8_t b;
        if (!c.read_u8(b)) return false;
        out = b;
        return true;
    }
    std::uint16_t w;
    if (!c.read_u16(w)) return false;
    out = w;
    return true;
}

/// Read the value-length per bits-4-3 + inline bits-2-0. Returns false on
/// underflow OR on the reserved-24-bit path if it cannot be satisfied.
bool read_length(Cursor& c,
                 LengthOfTheType_t bits43,
                 std::uint8_t inlineLow3,
                 std::uint32_t& outLen) {
    switch (bits43) {
        case TypeBits43_NoTypeLengthField_00:
            outLen = inlineLow3;
            return true;
        case TypeBits43_8BitsTypeLengthField_01: {
            std::uint8_t b;
            if (!c.read_u8(b)) return false;
            outLen = b;
            return true;
        }
        case TypeBits43_16BitsTypeLengthField_10: {
            std::uint16_t w;
            if (!c.read_u16(w)) return false;
            outLen = w;
            return true;
        }
        case TypeBits43_24BitsTypeLengthField_11:
            return c.read_u24(outLen);
    }
    return false;
}

/// Decode one record at the cursor. On success, advances the cursor past
/// the value bytes and appends a LwM2MObjectData to `out`. On any
/// under-run or unknown type, returns false without touching `out`.
bool decode_record(Cursor& c, LwM2MObject& out) {
    std::uint8_t hdr;
    if (!c.read_u8(hdr)) return false;

    auto bits76     = static_cast<TypeFieldOfTLV_t>      ((hdr >> 6) & 0b11);
    auto bit5       = static_cast<LengthOfTheIdentifier_t>((hdr >> 5) & 0b01);
    auto bits43     = static_cast<LengthOfTheType_t>     ((hdr >> 3) & 0b11);
    std::uint8_t lo = static_cast<std::uint8_t>           (hdr        & 0b111);

    std::uint32_t identifier = 0;
    if (!read_identifier(c, bit5, identifier)) return false;

    std::uint32_t valueLen = 0;
    if (!read_length(c, bits43, lo, valueLen)) return false;

    std::vector<std::uint8_t> value;
    if (!c.read_bytes(value, valueLen)) return false;

    switch (bits76) {
        case TypeBits76_ObjectInstance_OneOrMoreResourceTLV_00: {
            // Recurse on the contained bytes, carrying the Object Instance
            // ID forward in scratch. We mutate `out` directly so the
            // caller's flat record list grows in spec order.
            LwM2MObjectData scratch;
            scratch.m_oiid = identifier;
            Cursor inner(value.data(), value.size());
            while (!inner.eof()) {
                if (!decode_record(inner, out)) return false;
                // Inherit the Object Instance ID onto whichever record
                // decode_record just appended.
                if (!out.m_value.empty()) {
                    out.m_value.back().m_oiid = identifier;
                }
            }
            return true;
        }

        case TypeBits76_MultipleResource_OneOrMoreResourceInstanceTLV_10: {
            // The outer identifier is the Resource ID; each inner record is
            // a Resource Instance under it. The original code populated
            // data.m_rid here but lost it on recursion — fix per BUG-007
            // / REQ-ENC-001 multi-resource gap.
            Cursor inner(value.data(), value.size());
            while (!inner.eof()) {
                std::size_t before = out.m_value.size();
                if (!decode_record(inner, out)) return false;
                // Tag every record produced by this inner pass with the
                // Resource ID from the outer record.
                for (std::size_t i = before; i < out.m_value.size(); ++i) {
                    out.m_value[i].m_rid = identifier;
                }
            }
            return true;
        }

        case TypeBits76_ResourceInstance_OneOrMultipleResourceTLV_01: {
            LwM2MObjectData rec;
            rec.m_riid       = identifier;
            rec.m_ridlength  = valueLen;
            rec.m_ridvalue   = std::move(value);
            out.m_value.push_back(std::move(rec));
            return true;
        }

        case TypeBits76_ResourceWithValue_11: {
            LwM2MObjectData rec;
            rec.m_rid        = identifier;
            rec.m_ridlength  = valueLen;
            rec.m_ridvalue   = std::move(value);
            out.m_value.push_back(std::move(rec));
            return true;
        }
    }
    return false;
}

} // namespace

int decode_container(const std::string& payload,
                     LwM2MObjectData& /*scratch*/,
                     LwM2MObject& out) {
    if (payload.empty()) {
        return 0;
    }
    Cursor c(reinterpret_cast<const std::uint8_t*>(payload.data()),
             payload.size());

    while (!c.eof()) {
        if (!decode_record(c, out)) {
            // REQ-ENC-002: signal malformed input; do not throw.
            return -1;
        }
    }
    return 0;
}

/* ───────────────────────── encode ────────────────────────── */

namespace {

/// Compose the type byte from its four sub-fields.
inline std::uint8_t make_type(TypeFieldOfTLV_t bits76,
                              LengthOfTheIdentifier_t bit5,
                              LengthOfTheType_t bits43,
                              std::uint8_t low3) {
    return static_cast<std::uint8_t>(
        (static_cast<std::uint8_t>(bits76) << 6) |
        (static_cast<std::uint8_t>(bit5)   << 5) |
        (static_cast<std::uint8_t>(bits43) << 3) |
        (low3 & 0b111));
}

/// Emit `id` as 1 or 2 bytes (big-endian for 2-byte case).
void write_identifier(std::ostringstream& ss, std::uint16_t id) {
    if (id <= 0xFF) {
        std::uint8_t b = static_cast<std::uint8_t>(id);
        ss.write(reinterpret_cast<char*>(&b), 1);
    } else {
        std::uint16_t be = htons(id);
        ss.write(reinterpret_cast<char*>(&be), 2);
    }
}

inline LengthOfTheIdentifier_t identifier_width(std::uint16_t id) {
    return id <= 0xFF
        ? TypeBit5_LengthOfTheIdentifier8BitsLong_0
        : TypeBit5_LengthOfTheIdentifier16BitsLong_1;
}

/// Compute the length-field encoding for an arbitrary value length.
struct LenEnc {
    LengthOfTheType_t bits43;
    std::uint8_t      low3;
};

LenEnc length_encoding(std::size_t n) {
    if (n <= 7)        return {TypeBits43_NoTypeLengthField_00,
                               static_cast<std::uint8_t>(n)};
    if (n <= 0xFF)     return {TypeBits43_8BitsTypeLengthField_01,  0};
    if (n <= 0xFFFF)   return {TypeBits43_16BitsTypeLengthField_10, 0};
    return                    {TypeBits43_24BitsTypeLengthField_11, 0};
}

void write_length(std::ostringstream& ss, std::size_t n, LengthOfTheType_t bits43) {
    switch (bits43) {
        case TypeBits43_NoTypeLengthField_00:
            return;
        case TypeBits43_8BitsTypeLengthField_01: {
            std::uint8_t b = static_cast<std::uint8_t>(n);
            ss.write(reinterpret_cast<char*>(&b), 1);
            return;
        }
        case TypeBits43_16BitsTypeLengthField_10: {
            std::uint16_t w = htons(static_cast<std::uint16_t>(n));
            ss.write(reinterpret_cast<char*>(&w), 2);
            return;
        }
        case TypeBits43_24BitsTypeLengthField_11: {
            std::uint8_t b2 = (n >> 16) & 0xFF;
            std::uint8_t b1 = (n >>  8) & 0xFF;
            std::uint8_t b0 =  n        & 0xFF;
            ss.write(reinterpret_cast<char*>(&b2), 1);
            ss.write(reinterpret_cast<char*>(&b1), 1);
            ss.write(reinterpret_cast<char*>(&b0), 1);
            return;
        }
    }
}

} // namespace

int encode_string(TypeFieldOfTLV_t kind,
                  const std::string& value,
                  std::uint16_t id,
                  std::string& out) {
    auto lenEnc = length_encoding(value.size());
    auto type   = make_type(kind, identifier_width(id), lenEnc.bits43, lenEnc.low3);

    std::ostringstream ss;
    ss.write(reinterpret_cast<char*>(&type), 1);
    write_identifier(ss, id);
    write_length(ss, value.size(), lenEnc.bits43);
    ss.write(value.data(), value.size());
    out.assign(ss.str());
    return 0;
}

int encode_uint(TypeFieldOfTLV_t kind,
                std::uint32_t value,
                std::uint16_t id,
                std::string& out) {
    // Preserve the original behavior: width chosen by value magnitude;
    // length lives in the inline-3-bit field (NoTypeLengthField).
    std::uint8_t width;
    if      (value <= 0xFF)     width = 1;
    else if (value <= 0xFFFF)   width = 2;
    else                        width = 4;

    auto type = make_type(kind, identifier_width(id),
                          TypeBits43_NoTypeLengthField_00, width);

    std::ostringstream ss;
    ss.write(reinterpret_cast<char*>(&type), 1);
    write_identifier(ss, id);

    if (width == 1) {
        std::uint8_t b = static_cast<std::uint8_t>(value);
        ss.write(reinterpret_cast<char*>(&b), 1);
    } else if (width == 2) {
        std::uint16_t w = htons(static_cast<std::uint16_t>(value));
        ss.write(reinterpret_cast<char*>(&w), 2);
    } else {
        std::uint32_t l = htonl(value);
        ss.write(reinterpret_cast<char*>(&l), 4);
    }

    out.assign(ss.str());
    return 0;
}

int encode_time(TypeFieldOfTLV_t kind,
                std::int64_t value,
                std::uint16_t id,
                std::string& out) {
    // Annex C Time: signed seconds since the epoch, big-endian, minimum
    // width that preserves the sign. Match the int width rules from the
    // existing code path: choose by magnitude; never below 1 byte.
    std::uint64_t mag = value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1
                                  : static_cast<std::uint64_t>(value);
    std::uint8_t  width;
    if      (mag <= 0x7F)               width = 1;
    else if (mag <= 0x7FFF)             width = 2;
    else if (mag <= 0x7FFFFFFFULL)      width = 4;
    else                                width = 8;

    auto type = make_type(kind, identifier_width(id),
                          TypeBits43_NoTypeLengthField_00, width);

    std::ostringstream ss;
    ss.write(reinterpret_cast<char*>(&type), 1);
    write_identifier(ss, id);

    std::uint8_t buf[8];
    for (int i = width - 1; i >= 0; --i) {
        buf[i] = static_cast<std::uint8_t>(value & 0xFF);
        value >>= 8;
    }
    ss.write(reinterpret_cast<char*>(buf), width);

    out.assign(ss.str());
    return 0;
}

int encode_bool(TypeFieldOfTLV_t kind,
                bool value,
                std::uint16_t id,
                std::string& out) {
    auto type = make_type(kind, identifier_width(id),
                          TypeBits43_NoTypeLengthField_00, 1);

    std::ostringstream ss;
    ss.write(reinterpret_cast<char*>(&type), 1);
    write_identifier(ss, id);
    std::uint8_t b = value ? 0x01 : 0x00;
    ss.write(reinterpret_cast<char*>(&b), 1);

    out.assign(ss.str());
    return 0;
}

int encode_objlink(TypeFieldOfTLV_t kind,
                   std::uint16_t targetOid,
                   std::uint16_t targetIid,
                   std::uint16_t id,
                   std::string& out) {
    // Annex C ObjLink: two 16-bit big-endian fields, always 4 bytes.
    auto type = make_type(kind, identifier_width(id),
                          TypeBits43_NoTypeLengthField_00, 4);

    std::ostringstream ss;
    ss.write(reinterpret_cast<char*>(&type), 1);
    write_identifier(ss, id);

    std::uint16_t a = htons(targetOid);
    std::uint16_t b = htons(targetIid);
    ss.write(reinterpret_cast<char*>(&a), 2);
    ss.write(reinterpret_cast<char*>(&b), 2);

    out.assign(ss.str());
    return 0;
}

}} // namespace lwm2m::tlv
