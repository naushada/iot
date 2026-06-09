#ifndef __lwm2m_cert_chunk_hpp__
#define __lwm2m_cert_chunk_hpp__

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * @file lwm2m_cert_chunk.hpp
 * @brief Zip + chunk codec for pushing large cert artifacts over Object 2048.
 *
 * A cert PEM (~1.4–1.9 KB) exceeds a single DTLS record (tinydtls
 * DTLS_MAX_BUF = 1400, minus CoAP + DTLS overhead), so a server→device WRITE
 * of the whole PEM never arrives. This codec:
 *
 *   encode(pem): zip the PEM when it is larger than `zipThreshold`; if the
 *     (zipped) data fits one chunk it is a single opaque payload, otherwise it
 *     is split into chunks. Each returned string is the opaque value to WRITE
 *     to Object-2048 RID 1 of the artifact's instance.
 *
 *   Reassembler: the device feeds each received chunk; on the last one it
 *     returns the reassembled + inflated PEM.
 *
 * Wire framing of one opaque chunk:
 *   byte 0      flags   (bit0 = zipped)
 *   byte 1..2   seq     (chunk index, big-endian uint16)
 *   byte 3..4   total   (chunk count, big-endian uint16)
 *   byte 5..    chunk data
 */

namespace lwm2m {
namespace certchunk {

constexpr std::size_t kHeader = 5;        ///< flags(1) + seq(2) + total(2)

/// Encode `pem` into one or more opaque WRITE payloads. `maxChunkData` is the
/// data bytes per chunk (the opaque payload is kHeader + that); `zipThreshold`
/// is the PEM size above which the data is deflated. Never returns empty.
std::vector<std::string> encode(const std::string& pem,
                                std::size_t maxChunkData = 1019,
                                std::size_t zipThreshold = 1024);

/// Accumulates the chunks of one artifact (one Object-2048 instance).
class Reassembler {
public:
    /// Feed one received opaque chunk. Returns 1 = complete (`pem` filled),
    /// 0 = more chunks needed, -1 = malformed. Re-feeding a chunk is
    /// idempotent (re-pushes self-heal lost chunks).
    int feed(const std::string& chunk, std::string& pem);

    /// Forget partial state (e.g. on a total/zip mismatch — a new family).
    void reset();

private:
    std::vector<std::string> parts_;
    std::vector<bool>        got_;
    std::uint16_t            total_ = 0;
    bool                     zipped_ = false;
    std::size_t              have_  = 0;
};

}  // namespace certchunk
}  // namespace lwm2m

#endif /* __lwm2m_cert_chunk_hpp__ */
