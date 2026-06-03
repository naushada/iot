#include "data_store/proto.hpp"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace data_store::proto {

// ─────────────────────────────── opcodes ────────────────────────────

Op parse_op(std::uint16_t raw) {
    switch (raw) {
        case static_cast<std::uint16_t>(Op::Set):           return Op::Set;
        case static_cast<std::uint16_t>(Op::Get):           return Op::Get;
        case static_cast<std::uint16_t>(Op::RegisterWatch): return Op::RegisterWatch;
        case static_cast<std::uint16_t>(Op::RemoveWatch):   return Op::RemoveWatch;
        case static_cast<std::uint16_t>(Op::SchemaDump):    return Op::SchemaDump;
        case static_cast<std::uint16_t>(Op::NotifyEvent):   return Op::NotifyEvent;
        default:                                            return Op::Unknown;
    }
}

std::string op_name(Op op) {
    switch (op) {
        case Op::Set:           return "Set";
        case Op::Get:           return "Get";
        case Op::RegisterWatch: return "RegisterWatch";
        case Op::RemoveWatch:   return "RemoveWatch";
        case Op::SchemaDump:    return "SchemaDump";
        case Op::NotifyEvent:   return "NotifyEvent";
        case Op::Unknown:       break;
    }
    return "Unknown";
}

const char* status_name(Status s) {
    switch (s) {
        case Status::Ok:             return "Ok";
        case Status::BadFrame:       return "BadFrame";
        case Status::BadOpcode:      return "BadOpcode";
        case Status::BadPayload:     return "BadPayload";
        case Status::SchemaRejected: return "SchemaRejected";
        case Status::NotFound:       return "NotFound";
        case Status::InternalError:  return "InternalError";
        case Status::RateLimited:    return "RateLimited";
    }
    return "Unknown";
}

// ─────────────────────────────── header codec ───────────────────────

namespace {

inline void write_u16_be(std::uint16_t v, char* out) {
    out[0] = static_cast<char>((v >> 8) & 0xff);
    out[1] = static_cast<char>( v       & 0xff);
}

inline void write_u32_be(std::uint32_t v, char* out) {
    out[0] = static_cast<char>((v >> 24) & 0xff);
    out[1] = static_cast<char>((v >> 16) & 0xff);
    out[2] = static_cast<char>((v >>  8) & 0xff);
    out[3] = static_cast<char>( v        & 0xff);
}

inline std::uint16_t read_u16_be(const char* in) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint8_t>(in[0]) << 8) |
         static_cast<std::uint8_t>(in[1]));
}

inline std::uint32_t read_u32_be(const char* in) {
    return (static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[0])) << 24) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[1])) << 16) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[2])) <<  8) |
            static_cast<std::uint32_t>(static_cast<std::uint8_t>(in[3]));
}

void append_header(Op op, std::uint8_t type, std::uint8_t reqID,
                   std::uint32_t payload_size, std::string& out) {
    char hdr[kHeaderSize];
    write_u16_be(static_cast<std::uint16_t>(op), hdr);
    hdr[2] = static_cast<char>(type);
    hdr[3] = static_cast<char>(reqID);
    write_u32_be(payload_size, hdr + 4);
    out.append(hdr, kHeaderSize);
}

} // namespace

void encode_header(const Header& h, char* out) {
    write_u16_be(h.cmdID, out);
    out[2] = static_cast<char>(h.type);
    out[3] = static_cast<char>(h.reqID);
    write_u32_be(h.payload_size, out + 4);
}

void decode_header(const char* in, Header& h) {
    h.cmdID        = read_u16_be(in);
    h.type         = static_cast<std::uint8_t>(in[2]);
    h.reqID        = static_cast<std::uint8_t>(in[3]);
    h.payload_size = read_u32_be(in + 4);
}

// ─────────────────────────────── frame encode ───────────────────────

void encode_frame_command(Op op, std::uint8_t reqID,
                          std::string_view body, std::string& out) {
    append_header(op, /*type=*/0, reqID,
                  static_cast<std::uint32_t>(body.size()), out);
    out.append(body.data(), body.size());
}

void encode_frame_response(Op op, std::uint8_t reqID, Status st,
                           std::string_view body, std::string& out) {
    // Responses prefix the 2-byte big-endian status INSIDE the payload.
    const std::uint32_t plen = static_cast<std::uint32_t>(2 + body.size());
    append_header(op, /*type=*/Response, reqID, plen, out);
    char st_bytes[2];
    write_u16_be(static_cast<std::uint16_t>(st), st_bytes);
    out.append(st_bytes, 2);
    out.append(body.data(), body.size());
}

void encode_frame_push(Op op,
                       std::string_view body, std::string& out) {
    // No reqID correlation, no status prefix — payload IS the JSON body.
    append_header(op, /*type=*/Push, /*reqID=*/0,
                  static_cast<std::uint32_t>(body.size()), out);
    out.append(body.data(), body.size());
}

// ─────────────────────────────── frame decode ───────────────────────

bool try_decode_frame(std::string& buf,
                      Header& header,
                      std::string& payload) {
    if (buf.size() < kHeaderSize) return false;
    decode_header(buf.data(), header);
    if (header.payload_size > kMaxPayloadBytes) {
        throw std::runtime_error(
            "EMP payload_size " + std::to_string(header.payload_size) +
            " exceeds ceiling " + std::to_string(kMaxPayloadBytes));
    }
    const std::size_t need = kHeaderSize + header.payload_size;
    if (buf.size() < need) return false;
    payload.assign(buf.data() + kHeaderSize, header.payload_size);
    buf.erase(0, need);
    return true;
}

} // namespace data_store::proto
