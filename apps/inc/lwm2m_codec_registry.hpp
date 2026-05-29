#ifndef __lwm2m_codec_registry_hpp__
#define __lwm2m_codec_registry_hpp__

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include "lwm2m_codec_tlv.hpp"

/**
 * @file lwm2m_codec_registry.hpp
 * @brief Content-format-keyed registry of payload codecs.
 *
 * L1 deliverable per apps/docs/lwm2m-design.md §8 / REQ-ENC-008. Provides
 * the lookup point that CoAPAdapter::processRequest can use in L5/L6 to
 * dispatch a payload to the right codec without touching its own switch.
 *
 * v1 registers TLV (content-format 11542) only; SenML JSON (110) and SenML
 * CBOR (112) are added in L6.
 */

namespace lwm2m {

/// Numeric content-format codes (IANA / OMA registry). Centralised here so
/// adapters do not sprinkle magic numbers.
enum ContentFormat : std::uint16_t {
    CF_PlainText        = 0,
    CF_LinkFormat       = 40,
    CF_OctetStream      = 42,
    CF_Json             = 50,
    CF_SenmlJson        = 110,
    CF_SenmlCbor        = 112,
    CF_Cbor             = 60,
    CF_LwM2MTlv         = 11542,
    CF_LwM2MJson        = 11543,
    // Custom timeseries / uCBOR family (REQ-PUSH-002).
    CF_Timeseries       = 12119,
    CF_UCbor            = 12200,
    CF_UCborZ           = 12201,
    CF_SUCbor           = 12202,
    CF_SUCborZ          = 12203,
};

/**
 * @brief Abstract payload codec.
 *
 * Decode parses wire bytes into a LwM2MObject; encode serialises one back.
 * Higher-level conversions (LwM2MObject → ObjectStore::Resource and back)
 * live in lwm2m_adapter; codecs only deal with the wire shape so a new
 * format added in L6 only needs to implement this interface.
 */
class ICodec {
public:
    virtual ~ICodec() = default;

    virtual ContentFormat format() const = 0;

    /// 0 on success, -1 on malformed input (REQ-ENC-002).
    virtual int decode(const std::string& payload, LwM2MObject& out) = 0;

    /// 0 on success. Empty `payload` is permitted; the codec writes nothing.
    virtual int encode(const LwM2MObject& in, std::string& payload) = 0;
};

/**
 * @brief Singleton-style codec lookup keyed by content-format code.
 *
 * Threading: registry mutation happens once at startup before the reactor
 * thread is spawned, so no lock is required on lookup.
 */
class CodecRegistry {
public:
    static CodecRegistry& instance();

    /// Register a codec under its declared format(). Replaces any prior
    /// entry for that format code (registration order = priority).
    void add(std::shared_ptr<ICodec> codec);

    /// nullptr if no codec is registered for `cf`.
    ICodec* find(ContentFormat cf);
    ICodec* find(std::uint16_t cf) {
        return find(static_cast<ContentFormat>(cf));
    }

    /// Test hook — clears the registry. Production code never calls this.
    void clear() { m_codecs.clear(); }

private:
    CodecRegistry() = default;
    std::unordered_map<std::uint16_t, std::shared_ptr<ICodec>> m_codecs;
};

/// TLV codec implementation. Lives in lwm2m_codec_registry.cpp because the
/// implementation is trivial (it just delegates into lwm2m::tlv free
/// functions); separate translation unit would be overkill.
class TlvCodec : public ICodec {
public:
    ContentFormat format() const override { return CF_LwM2MTlv; }

    int decode(const std::string& payload, LwM2MObject& out) override {
        LwM2MObjectData scratch;
        return ::lwm2m::tlv::decode_container(payload, scratch, out);
    }

    int encode(const LwM2MObject& /*in*/, std::string& /*payload*/) override {
        // L1 stub: encode-from-LwM2MObject is not driven yet — bootstrap
        // payloads still go through LwM2MAdapter::buildLwM2MPayload which
        // uses the per-RID encoders directly. L5 wires this up when
        // Read/Write start producing/consuming LwM2MObject values.
        return 0;
    }
};

/**
 * @brief SenML JSON / SenML CBOR adapters around the namespace-level codec.
 *
 * The ICodec interface speaks LwM2MObject (the TLV wire shape). SenML
 * carries type-rich records (numeric / string / boolean / opaque), so
 * type-preserving SenML use goes through `lwm2m::senml::encode_json` /
 * `decode_json` / `encode_cbor` / `decode_cbor` directly. These adapter
 * shims exist so other code can discover SenML support via the registry
 * (CF=110, CF=112) and refuse 4.15 when neither is registered.
 *
 * The adapter `decode()` stores each SenML record's value as raw bytes
 * inside `LwM2MObjectData.m_ridvalue`, with `m_rid` interpreted from the
 * SenML name when it parses as an integer; the lossy nature of this
 * shape is why the DM layer routes SenML directly to the namespace
 * functions, not through `ICodec`.
 */
class SenmlJsonCodec : public ICodec {
public:
    ContentFormat format() const override { return CF_SenmlJson; }
    int decode(const std::string& payload, LwM2MObject& out) override;
    int encode(const LwM2MObject& in, std::string& payload) override;
};

class SenmlCborCodec : public ICodec {
public:
    ContentFormat format() const override { return CF_SenmlCbor; }
    int decode(const std::string& payload, LwM2MObject& out) override;
    int encode(const LwM2MObject& in, std::string& payload) override;
};

} // namespace lwm2m

#endif /*__lwm2m_codec_registry_hpp__*/
