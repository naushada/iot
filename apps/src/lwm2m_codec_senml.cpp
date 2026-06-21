#include "lwm2m_codec_senml.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

#include "nlohmann/json.hpp"

namespace lwm2m { namespace senml {

using json = nlohmann::json;

namespace {

/* ────────── base64url (RFC 4648 §5) for SenML vd JSON shape ────────── */

const char b64u_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string base64url_encode(const std::string& in) {
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= in.size()) {
        std::uint32_t v = (static_cast<std::uint8_t>(in[i])     << 16) |
                          (static_cast<std::uint8_t>(in[i + 1]) <<  8) |
                           static_cast<std::uint8_t>(in[i + 2]);
        out.push_back(b64u_alphabet[(v >> 18) & 0x3F]);
        out.push_back(b64u_alphabet[(v >> 12) & 0x3F]);
        out.push_back(b64u_alphabet[(v >>  6) & 0x3F]);
        out.push_back(b64u_alphabet[ v        & 0x3F]);
        i += 3;
    }
    if (i + 1 == in.size()) {
        std::uint32_t v = static_cast<std::uint8_t>(in[i]) << 16;
        out.push_back(b64u_alphabet[(v >> 18) & 0x3F]);
        out.push_back(b64u_alphabet[(v >> 12) & 0x3F]);
    } else if (i + 2 == in.size()) {
        std::uint32_t v = (static_cast<std::uint8_t>(in[i])     << 16) |
                          (static_cast<std::uint8_t>(in[i + 1]) <<  8);
        out.push_back(b64u_alphabet[(v >> 18) & 0x3F]);
        out.push_back(b64u_alphabet[(v >> 12) & 0x3F]);
        out.push_back(b64u_alphabet[(v >>  6) & 0x3F]);
    }
    return out;
}

int base64url_decode(const std::string& in, std::string& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-') return 62;
        if (c == '_') return 63;
        return -1;
    };
    out.clear();
    out.reserve((in.size() * 3 + 3) / 4);
    std::size_t i = 0;
    while (i < in.size()) {
        int a = val(in[i]);
        int b = i + 1 < in.size() ? val(in[i + 1]) : -1;
        int c = i + 2 < in.size() ? val(in[i + 2]) : -1;
        int d = i + 3 < in.size() ? val(in[i + 3]) : -1;
        if (a < 0 || b < 0) return -1;
        out.push_back(static_cast<char>((a << 2) | (b >> 4)));
        if (c < 0) break;
        out.push_back(static_cast<char>(((b & 0x0F) << 4) | (c >> 2)));
        if (d < 0) break;
        out.push_back(static_cast<char>(((c & 0x03) << 6) | d));
        i += 4;
    }
    return 0;
}

} // namespace

/* ──────────────────────────── JSON ──────────────────────────────────── */

std::string encode_json(const std::vector<Record>& records) {
    if (records.empty()) return "[]";
    const std::string& bn = records.front().baseName;
    for (const auto& r : records) {
        if (r.baseName != bn) return {};      // bn must be consistent
    }

    // Base Time, like bn, is the first record's and must be consistent.
    const bool   hasBt = records.front().hasBaseTime;
    const double bt    = records.front().baseTime;
    for (const auto& r : records) {
        if (r.hasBaseTime != hasBt || (hasBt && r.baseTime != bt)) return {};
    }

    json out = json::array();
    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& r = records[i];
        json rec = json::object();
        if (i == 0 && !bn.empty()) rec["bn"] = bn;
        if (i == 0 && hasBt)       rec["bt"] = bt;
        if (!r.name.empty()) rec["n"] = r.name;
        if (r.hasTime)       rec["t"] = r.time;
        switch (r.kind) {
            case ValueKind::Numeric:
                if (r.isFloat) rec["v"] = r.numericValue;
                else           rec["v"] = static_cast<std::int64_t>(r.numericValue);
                break;
            case ValueKind::String:  rec["vs"] = r.stringValue;             break;
            case ValueKind::Boolean: rec["vb"] = r.booleanValue;            break;
            case ValueKind::Data:    rec["vd"] = base64url_encode(r.dataValue); break;
            case ValueKind::None:    break;     // record carries name only
        }
        out.push_back(std::move(rec));
    }
    return out.dump();
}

int decode_json(const std::string& bytes, std::vector<Record>& out) {
    out.clear();
    json doc;
    try {
        doc = json::parse(bytes);
    } catch (...) {
        return -1;
    }
    if (!doc.is_array()) return -1;

    std::string bn;
    double bt = 0.0;
    bool   hasBt = false;
    for (std::size_t i = 0; i < doc.size(); ++i) {
        const auto& rec = doc.at(i);
        if (!rec.is_object()) return -1;

        if (rec.contains("bn")) {
            if (i != 0 || !rec["bn"].is_string()) return -1;
            bn = rec["bn"].get<std::string>();
        }
        if (rec.contains("bt")) {
            if (i != 0 || !rec["bt"].is_number()) return -1;
            bt = rec["bt"].get<double>();
            hasBt = true;
        }

        Record r;
        r.baseName = bn;
        r.baseTime = bt;
        r.hasBaseTime = hasBt;
        if (rec.contains("n")) {
            if (!rec["n"].is_string()) return -1;
            r.name = rec["n"].get<std::string>();
        }
        if (rec.contains("t")) {
            if (!rec["t"].is_number()) return -1;
            r.time = rec["t"].get<double>();
            r.hasTime = true;
        }

        int valueCount = 0;
        if (rec.contains("v")) {
            ++valueCount;
            if (!rec["v"].is_number()) return -1;
            r.kind = ValueKind::Numeric;
            r.isFloat = rec["v"].is_number_float();
            r.numericValue = rec["v"].get<double>();
        }
        if (rec.contains("vs")) {
            ++valueCount;
            if (!rec["vs"].is_string()) return -1;
            r.kind = ValueKind::String;
            r.stringValue = rec["vs"].get<std::string>();
        }
        if (rec.contains("vb")) {
            ++valueCount;
            if (!rec["vb"].is_boolean()) return -1;
            r.kind = ValueKind::Boolean;
            r.booleanValue = rec["vb"].get<bool>();
        }
        if (rec.contains("vd")) {
            ++valueCount;
            if (!rec["vd"].is_string()) return -1;
            r.kind = ValueKind::Data;
            if (base64url_decode(rec["vd"].get<std::string>(), r.dataValue) != 0) {
                return -1;
            }
        }
        if (valueCount > 1) return -1;

        out.push_back(std::move(r));
    }
    return 0;
}

/* ──────────────────────────── CBOR ──────────────────────────────────── */

namespace {

/* Major types from RFC 8949 §3.1. */
constexpr std::uint8_t CBOR_UINT     = 0;
constexpr std::uint8_t CBOR_NINT     = 1;
constexpr std::uint8_t CBOR_BYTES    = 2;
constexpr std::uint8_t CBOR_TEXT     = 3;
constexpr std::uint8_t CBOR_ARRAY    = 4;
constexpr std::uint8_t CBOR_MAP      = 5;
constexpr std::uint8_t CBOR_SIMPLE   = 7;
constexpr std::uint8_t CBOR_FALSE    = 0xF4;
constexpr std::uint8_t CBOR_TRUE     = 0xF5;
constexpr std::uint8_t CBOR_F64      = 0xFB;

/* SenML CBOR integer labels per RFC 8428 §6. */
constexpr int LBL_BN = -2;
constexpr int LBL_BT = -3;
constexpr int LBL_N  =  0;
constexpr int LBL_T  =  6;
constexpr int LBL_V  =  2;
constexpr int LBL_VS =  3;
constexpr int LBL_VB =  4;
constexpr int LBL_VD =  8;

void emit_head(std::string& out, std::uint8_t major, std::uint64_t arg) {
    std::uint8_t mt = major << 5;
    if (arg < 24) {
        out.push_back(static_cast<char>(mt | static_cast<std::uint8_t>(arg)));
        return;
    }
    if (arg <= 0xFF) {
        out.push_back(static_cast<char>(mt | 24));
        out.push_back(static_cast<char>(arg));
        return;
    }
    if (arg <= 0xFFFF) {
        out.push_back(static_cast<char>(mt | 25));
        out.push_back(static_cast<char>((arg >> 8) & 0xFF));
        out.push_back(static_cast<char>(arg & 0xFF));
        return;
    }
    if (arg <= 0xFFFFFFFFULL) {
        out.push_back(static_cast<char>(mt | 26));
        for (int s = 24; s >= 0; s -= 8) {
            out.push_back(static_cast<char>((arg >> s) & 0xFF));
        }
        return;
    }
    out.push_back(static_cast<char>(mt | 27));
    for (int s = 56; s >= 0; s -= 8) {
        out.push_back(static_cast<char>((arg >> s) & 0xFF));
    }
}

void emit_int(std::string& out, int v) {
    if (v >= 0) emit_head(out, CBOR_UINT, static_cast<std::uint64_t>(v));
    else        emit_head(out, CBOR_NINT, static_cast<std::uint64_t>(-1 - v));
}

void emit_text(std::string& out, const std::string& s) {
    emit_head(out, CBOR_TEXT, s.size());
    out.append(s);
}

void emit_bytes(std::string& out, const std::string& s) {
    emit_head(out, CBOR_BYTES, s.size());
    out.append(s);
}

void emit_double(std::string& out, double v) {
    out.push_back(static_cast<char>(CBOR_F64));
    std::uint64_t bits;
    static_assert(sizeof(bits) == sizeof(v), "double size mismatch");
    std::memcpy(&bits, &v, sizeof(bits));
    for (int s = 56; s >= 0; s -= 8) {
        out.push_back(static_cast<char>((bits >> s) & 0xFF));
    }
}

/// Time (`bt`/`t`): emit as a compact CBOR unsigned int when the value is a
/// non-negative whole number (the usual unix-seconds case), else as a float64.
/// Round-trips losslessly either way (decode reads both via read_number).
void emit_time(std::string& out, double t) {
    std::int64_t it = static_cast<std::int64_t>(t);
    if (it >= 0 && static_cast<double>(it) == t) {
        emit_head(out, CBOR_UINT, static_cast<std::uint64_t>(it));
    } else {
        emit_double(out, t);
    }
}

class CborCursor {
public:
    CborCursor(const std::uint8_t* p, std::size_t n) : m_p(p), m_n(n), m_i(0) {}

    bool read_head(std::uint8_t& major, std::uint64_t& arg) {
        if (m_i >= m_n) return false;
        std::uint8_t b = m_p[m_i++];
        major = b >> 5;
        std::uint8_t ai = b & 0x1F;
        if (ai < 24) { arg = ai; return true; }
        std::size_t len = 0;
        switch (ai) {
            case 24: len = 1; break;
            case 25: len = 2; break;
            case 26: len = 4; break;
            case 27: len = 8; break;
            default: return false;     // indefinite-length not supported
        }
        if (m_i + len > m_n) return false;
        arg = 0;
        for (std::size_t k = 0; k < len; ++k) {
            arg = (arg << 8) | m_p[m_i + k];
        }
        m_i += len;
        return true;
    }

    bool read_bytes(std::string& out, std::size_t n) {
        if (m_i + n > m_n) return false;
        out.assign(reinterpret_cast<const char*>(m_p + m_i), n);
        m_i += n;
        return true;
    }

    bool read_double(double& out) {
        if (m_i >= m_n) return false;
        std::uint8_t b = m_p[m_i++];
        if (b == CBOR_F64) {
            if (m_i + 8 > m_n) return false;
            std::uint64_t bits = 0;
            for (int k = 0; k < 8; ++k) bits = (bits << 8) | m_p[m_i + k];
            m_i += 8;
            std::memcpy(&out, &bits, sizeof(out));
            return true;
        }
        // Half (0xF9) and single (0xFA) floats: not needed for the
        // SenML CBOR profile we generate; treat as malformed for v1.
        return false;
    }

    bool read_bool(bool& out) {
        if (m_i >= m_n) return false;
        std::uint8_t b = m_p[m_i++];
        if (b == CBOR_TRUE)  { out = true;  return true; }
        if (b == CBOR_FALSE) { out = false; return true; }
        return false;
    }

    bool peek_is_double() const {
        return m_i < m_n && m_p[m_i] == CBOR_F64;
    }
    bool peek_is_bool() const {
        return m_i < m_n && (m_p[m_i] == CBOR_TRUE || m_p[m_i] == CBOR_FALSE);
    }

    std::size_t pos() const { return m_i; }
    std::size_t size() const { return m_n; }
    bool eof() const { return m_i >= m_n; }

private:
    const std::uint8_t* m_p;
    std::size_t         m_n;
    std::size_t         m_i;
};

bool read_text(CborCursor& c, std::string& out) {
    std::uint8_t major;
    std::uint64_t arg;
    if (!c.read_head(major, arg)) return false;
    if (major != CBOR_TEXT) return false;
    return c.read_bytes(out, static_cast<std::size_t>(arg));
}

bool read_signed_int(CborCursor& c, std::int64_t& out) {
    std::uint8_t major;
    std::uint64_t arg;
    if (!c.read_head(major, arg)) return false;
    if (major == CBOR_UINT) { out = static_cast<std::int64_t>(arg); return true; }
    if (major == CBOR_NINT) { out = -1 - static_cast<std::int64_t>(arg); return true; }
    return false;
}

/// Read a SenML time/number that may be encoded as a CBOR int OR a float64
/// (emit_time picks the compact form). Always yields a double.
bool read_number(CborCursor& c, double& out) {
    if (c.peek_is_double()) return c.read_double(out);
    std::int64_t v;
    if (!read_signed_int(c, v)) return false;
    out = static_cast<double>(v);
    return true;
}

bool skip_value(CborCursor& c) {
    std::uint8_t major;
    std::uint64_t arg;
    auto savePos = c.pos();
    (void)savePos;
    if (!c.read_head(major, arg)) return false;
    switch (major) {
        case CBOR_UINT: case CBOR_NINT: return true;
        case CBOR_BYTES: case CBOR_TEXT: {
            std::string sink;
            return c.read_bytes(sink, arg);
        }
        case CBOR_ARRAY:
            for (std::uint64_t k = 0; k < arg; ++k) if (!skip_value(c)) return false;
            return true;
        case CBOR_MAP:
            for (std::uint64_t k = 0; k < arg; ++k) {
                if (!skip_value(c)) return false;   // key
                if (!skip_value(c)) return false;   // value
            }
            return true;
        case CBOR_SIMPLE:
            return true;
        default:
            return false;
    }
}

} // namespace

std::string encode_cbor(const std::vector<Record>& records) {
    if (records.empty()) {
        std::string out;
        emit_head(out, CBOR_ARRAY, 0);
        return out;
    }
    const std::string& bn = records.front().baseName;
    for (const auto& r : records) {
        if (r.baseName != bn) return {};
    }
    // Base Time, like bn, is the first record's and must be consistent.
    const bool   hasBt = records.front().hasBaseTime;
    const double bt    = records.front().baseTime;
    for (const auto& r : records) {
        if (r.hasBaseTime != hasBt || (hasBt && r.baseTime != bt)) return {};
    }

    std::string out;
    emit_head(out, CBOR_ARRAY, records.size());

    for (std::size_t i = 0; i < records.size(); ++i) {
        const auto& r = records[i];
        std::size_t entries = 0;
        if (i == 0 && !bn.empty()) ++entries;
        if (i == 0 && hasBt)       ++entries;
        if (!r.name.empty()) ++entries;
        if (r.hasTime)       ++entries;
        if (r.kind != ValueKind::None) ++entries;

        emit_head(out, CBOR_MAP, entries);

        if (i == 0 && !bn.empty()) {
            emit_int(out, LBL_BN); emit_text(out, bn);
        }
        if (i == 0 && hasBt) {
            emit_int(out, LBL_BT); emit_time(out, bt);
        }
        if (!r.name.empty()) {
            emit_int(out, LBL_N); emit_text(out, r.name);
        }
        if (r.hasTime) {
            emit_int(out, LBL_T); emit_time(out, r.time);
        }
        switch (r.kind) {
            case ValueKind::Numeric:
                emit_int(out, LBL_V);
                if (r.isFloat) {
                    emit_double(out, r.numericValue);
                } else {
                    std::int64_t v = static_cast<std::int64_t>(r.numericValue);
                    emit_int(out, static_cast<int>(v));     // SenML v ≤ 32 bits fits
                }
                break;
            case ValueKind::String:
                emit_int(out, LBL_VS); emit_text(out, r.stringValue);
                break;
            case ValueKind::Boolean:
                emit_int(out, LBL_VB);
                out.push_back(static_cast<char>(r.booleanValue ? CBOR_TRUE : CBOR_FALSE));
                break;
            case ValueKind::Data:
                emit_int(out, LBL_VD); emit_bytes(out, r.dataValue);
                break;
            case ValueKind::None: break;
        }
    }
    return out;
}

int decode_cbor(const std::string& bytes, std::vector<Record>& out) {
    out.clear();
    CborCursor c(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());

    std::uint8_t major;
    std::uint64_t nRecs;
    if (!c.read_head(major, nRecs)) return -1;
    if (major != CBOR_ARRAY) return -1;

    std::string bn;
    double bt = 0.0;
    bool   hasBt = false;
    for (std::uint64_t i = 0; i < nRecs; ++i) {
        std::uint8_t m;
        std::uint64_t nFields;
        if (!c.read_head(m, nFields)) return -1;
        if (m != CBOR_MAP) return -1;

        Record r;
        int valueCount = 0;
        for (std::uint64_t f = 0; f < nFields; ++f) {
            std::int64_t key;
            if (!read_signed_int(c, key)) return -1;
            switch (key) {
                case LBL_BN: {
                    if (i != 0) return -1;
                    if (!read_text(c, bn)) return -1;
                    break;
                }
                case LBL_BT: {
                    if (i != 0) return -1;
                    if (!read_number(c, bt)) return -1;
                    hasBt = true;
                    break;
                }
                case LBL_T:
                    if (!read_number(c, r.time)) return -1;
                    r.hasTime = true;
                    break;
                case LBL_N:
                    if (!read_text(c, r.name)) return -1;
                    break;
                case LBL_V: {
                    ++valueCount;
                    r.kind = ValueKind::Numeric;
                    if (c.peek_is_double()) {
                        if (!c.read_double(r.numericValue)) return -1;
                        r.isFloat = true;
                    } else {
                        std::int64_t v;
                        if (!read_signed_int(c, v)) return -1;
                        r.numericValue = static_cast<double>(v);
                        r.isFloat = false;
                    }
                    break;
                }
                case LBL_VS:
                    ++valueCount;
                    r.kind = ValueKind::String;
                    if (!read_text(c, r.stringValue)) return -1;
                    break;
                case LBL_VB:
                    ++valueCount;
                    r.kind = ValueKind::Boolean;
                    if (!c.read_bool(r.booleanValue)) return -1;
                    break;
                case LBL_VD: {
                    ++valueCount;
                    r.kind = ValueKind::Data;
                    std::uint8_t mb;
                    std::uint64_t blen;
                    if (!c.read_head(mb, blen)) return -1;
                    if (mb != CBOR_BYTES) return -1;
                    if (!c.read_bytes(r.dataValue, static_cast<std::size_t>(blen))) return -1;
                    break;
                }
                default:
                    // Unknown label — skip per RFC 8428 §4.1 forward
                    // compatibility expectation.
                    if (!skip_value(c)) return -1;
                    break;
            }
        }
        if (valueCount > 1) return -1;
        r.baseName = bn;
        r.baseTime = bt;
        r.hasBaseTime = hasBt;
        out.push_back(std::move(r));
    }
    return 0;
}

}} // namespace lwm2m::senml
