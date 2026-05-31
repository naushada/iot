#ifndef __data_store_proto_hpp__
#define __data_store_proto_hpp__

/// Embedded Micro Protocol (EMP) framing for the data-store wire.
///
/// Modelled on Eclipse Mihini's EMP
/// (https://wiki.eclipse.org/Mihini/Embedded_Micro_Protocol):
/// fixed 8-byte big-endian header followed by a JSON payload.
///
///   offset  size  field
///   ──────  ────  ─────────────────────────────────────────────
///   0       2     cmdID            opcode (see Op below)
///   2       1     type             frame kind, see FrameTypeBit
///   3       1     reqID            request correlator, 0..255
///   4       4     payload_size     big-endian, bytes of body
///   8       N     payload          UTF-8 JSON document (or empty)
///
/// Type byte:
///   bit 0 = 0  → Command  (client → server)
///   bit 0 = 1  → Response (server → client, correlated by reqID)
///   bit 1 = 1  → Push     (server → client, reqID = 0, no correlation;
///                          repurposes Mihini's reserved bit 1 for
///                          server-initiated notify, see protocol.md)
///   bits 2..7 reserved (must be 0).
///
/// Responses carry a 2-byte big-endian `status` prefix INSIDE the
/// payload (i.e. the JSON body, if any, starts at payload offset 2).
/// status == 0 → success; any other value indicates failure, with the
/// JSON body OPTIONALLY carrying `{"err":"..."}`. Pushes do NOT carry
/// a status prefix — their full payload is JSON.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace data_store::proto {

// ─────────────────────────────── Opcodes ────────────────────────────

enum class Op : std::uint16_t {
    Unknown       = 0x0000,
    Set           = 0x0001,   ///< Client → Server: write keys
    Get           = 0x0002,   ///< Client → Server: read keys
    RegisterWatch = 0x0003,   ///< Client → Server: subscribe keys
    RemoveWatch   = 0x0004,   ///< Client → Server: unsubscribe keys
    NotifyEvent   = 0x0064,   ///< Server → Client (push): value changed
};

Op          parse_op(std::uint16_t raw);
std::string op_name(Op op);

// ─────────────────────────────── Frame type ─────────────────────────

enum FrameTypeBit : std::uint8_t {
    Response = 0x01,    ///< bit 0
    Push     = 0x02,    ///< bit 1 — our extension of Mihini's reserved
};

inline bool is_command (std::uint8_t t) { return (t & (Response | Push)) == 0; }
inline bool is_response(std::uint8_t t) { return (t & Response) != 0; }
inline bool is_push    (std::uint8_t t) { return (t & Push)     != 0; }

// ─────────────────────────────── Status codes ───────────────────────

enum class Status : std::uint16_t {
    Ok              = 0,
    BadFrame        = 0x8001,   ///< malformed header / truncated payload
    BadOpcode       = 0x8002,   ///< unknown / unsupported cmdID
    BadPayload      = 0x8003,   ///< payload is not the JSON shape we expect
    SchemaRejected  = 0x8004,   ///< SchemaRegistry::validate_set said no
    NotFound        = 0x8005,   ///< (reserved for future single-key reads)
    InternalError   = 0x8006,   ///< caught exception on the server side
};

const char* status_name(Status s);

// ─────────────────────────────── Header (POD) ───────────────────────

struct Header {
    std::uint16_t cmdID        = 0;
    std::uint8_t  type         = 0;
    std::uint8_t  reqID        = 0;
    std::uint32_t payload_size = 0;
};

constexpr std::size_t kHeaderSize = 8;

/// Big-endian encode/decode of the 8-byte header. The helpers assume
/// `out` has room for kHeaderSize bytes and `in` has at least
/// kHeaderSize bytes available — callers gate on `buf.size() >= 8`.
void encode_header(const Header& h, char* out);
void decode_header(const char* in, Header& h);

/// Append a complete frame (header + status-prefix-if-response + body)
/// to `out`. `body` is the JSON document (or "") to ship. Responses
/// stamp the 2-byte status BEFORE the body inside the payload.
void encode_frame_command (Op op, std::uint8_t reqID,
                           std::string_view body, std::string& out);
void encode_frame_response(Op op, std::uint8_t reqID, Status st,
                           std::string_view body, std::string& out);
void encode_frame_push    (Op op,
                           std::string_view body, std::string& out);

/// Pull one complete frame off the front of `buf`. Returns:
///   * true  — `header` + `payload` filled, frame removed from `buf`
///   * false — not enough bytes yet (caller waits for more recv)
/// On a malformed header (oversized payload past kMaxPayloadBytes)
/// throws std::runtime_error so the caller can drop the connection.
bool try_decode_frame(std::string& buf,
                      Header& header,
                      std::string& payload);

/// Maximum accepted payload size. Hard ceiling so a malicious / buggy
/// peer can't make us allocate gigabytes. 1 MiB is far more than any
/// real Set/Get carrying a handful of keys needs.
constexpr std::uint32_t kMaxPayloadBytes = 1u << 20;

// ─────────────────────────────── Endpoint defaults ──────────────────

constexpr const char* kDefaultSocketPath = "/var/run/iot/data_store.sock";
constexpr const char* kDefaultStorePath  = "/var/lib/iot/data_store.lua";

/// Conventional drop-in directory for client-published *.lua schema
/// files. ds-server auto-loads it on startup when `ds-schema-dir=`
/// isn't set explicitly; a missing directory is silently treated as
/// "no schemas" so the daemon still boots on dev machines.
constexpr const char* kDefaultSchemaDir  = "/etc/iot/ds-schemas/";

} // namespace data_store::proto

#endif /* __data_store_proto_hpp__ */
