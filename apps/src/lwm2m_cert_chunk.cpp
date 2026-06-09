#include "lwm2m_cert_chunk.hpp"

#include <cstring>
#include <zlib.h>

namespace lwm2m {
namespace certchunk {

namespace {

inline std::uint8_t u8(char c) { return static_cast<std::uint8_t>(c); }

// Deflate `in` → `out` (zlib stream, so the device can inflate without
// knowing the original size). Returns false on failure.
bool zip(const std::string& in, std::string& out) {
    uLongf bound = ::compressBound(static_cast<uLong>(in.size()));
    out.resize(bound);
    uLongf dest = bound;
    int rc = ::compress2(reinterpret_cast<Bytef*>(&out[0]), &dest,
                         reinterpret_cast<const Bytef*>(in.data()),
                         static_cast<uLong>(in.size()), Z_BEST_COMPRESSION);
    if (rc != Z_OK) return false;
    out.resize(dest);
    return true;
}

// Streaming inflate — no need for the original size up front.
bool unzip(const std::string& in, std::string& out) {
    z_stream s;
    std::memset(&s, 0, sizeof(s));
    if (::inflateInit(&s) != Z_OK) return false;
    s.next_in  = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    s.avail_in = static_cast<uInt>(in.size());
    out.clear();
    char buf[4096];
    int rc;
    do {
        s.next_out  = reinterpret_cast<Bytef*>(buf);
        s.avail_out = sizeof(buf);
        rc = ::inflate(&s, Z_NO_FLUSH);
        if (rc != Z_OK && rc != Z_STREAM_END) { ::inflateEnd(&s); return false; }
        out.append(buf, sizeof(buf) - s.avail_out);
    } while (rc != Z_STREAM_END);
    ::inflateEnd(&s);
    return true;
}

}  // namespace

std::vector<std::string> encode(const std::string& pem,
                                std::size_t maxChunkData,
                                std::size_t zipThreshold) {
    if (maxChunkData == 0) maxChunkData = 1;

    bool zipped = false;
    std::string data = pem;
    if (pem.size() > zipThreshold) {
        std::string z;
        if (zip(pem, z) && z.size() < pem.size()) { data.swap(z); zipped = true; }
    }

    std::size_t total = data.empty() ? 1
                                     : (data.size() + maxChunkData - 1) / maxChunkData;
    if (total > 0xFFFF) total = 0xFFFF;   // wire field is uint16

    std::vector<std::string> out;
    out.reserve(total);
    for (std::size_t i = 0; i < total; ++i) {
        std::string f;
        f.reserve(kHeader + maxChunkData);
        f.push_back(static_cast<char>(zipped ? 1 : 0));
        f.push_back(static_cast<char>((i >> 8) & 0xFF));
        f.push_back(static_cast<char>(i & 0xFF));
        f.push_back(static_cast<char>((total >> 8) & 0xFF));
        f.push_back(static_cast<char>(total & 0xFF));
        if (!data.empty()) f.append(data, i * maxChunkData, maxChunkData);
        out.push_back(std::move(f));
    }
    return out;
}

void Reassembler::reset() {
    parts_.clear();
    got_.clear();
    total_ = 0;
    zipped_ = false;
    have_  = 0;
}

int Reassembler::feed(const std::string& chunk, std::string& pem) {
    if (chunk.size() < kHeader) return -1;
    const bool          z    = (u8(chunk[0]) & 1) != 0;
    const std::uint16_t seq  = static_cast<std::uint16_t>((u8(chunk[1]) << 8) | u8(chunk[2]));
    const std::uint16_t tot  = static_cast<std::uint16_t>((u8(chunk[3]) << 8) | u8(chunk[4]));
    if (tot == 0 || seq >= tot) return -1;

    // First chunk, or a re-pushed cert with a different shape → start fresh.
    if (total_ != tot || zipped_ != z || parts_.size() != tot) {
        reset();
        total_  = tot;
        zipped_ = z;
        parts_.assign(tot, std::string());
        got_.assign(tot, false);
    }

    if (!got_[seq]) { got_[seq] = true; ++have_; }
    parts_[seq].assign(chunk, kHeader, std::string::npos);

    if (have_ < total_) return 0;   // still missing chunks

    std::string data;
    for (auto& p : parts_) data += p;
    if (zipped_) {
        if (!unzip(data, pem)) { reset(); return -1; }
    } else {
        pem.swap(data);
    }
    reset();
    return 1;
}

}  // namespace certchunk
}  // namespace lwm2m
