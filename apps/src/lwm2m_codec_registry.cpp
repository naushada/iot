#include "lwm2m_codec_registry.hpp"

#include <cctype>
#include <string>

#include "lwm2m_codec_senml.hpp"

namespace lwm2m {

/* ────────── SenML adapter shims ────────── */

namespace {

/// Best-effort: if the SenML name parses to an integer, treat it as a
/// RID. Otherwise leave m_rid at 0 — callers that need richer type
/// preservation should go through ::lwm2m::senml directly.
bool parse_int_name(const std::string& n, std::uint32_t& out) {
    if (n.empty()) return false;
    for (char c : n) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    out = static_cast<std::uint32_t>(std::stoul(n));
    return true;
}

void senml_to_lwm2m(const std::vector<senml::Record>& recs, LwM2MObject& out) {
    for (const auto& r : recs) {
        LwM2MObjectData d;
        std::uint32_t rid = 0;
        if (parse_int_name(r.name, rid)) d.m_rid = rid;
        std::string v;
        switch (r.kind) {
            case senml::ValueKind::Numeric:
                v = r.isFloat ? std::to_string(r.numericValue)
                              : std::to_string(static_cast<std::int64_t>(r.numericValue));
                break;
            case senml::ValueKind::String:  v = r.stringValue; break;
            case senml::ValueKind::Boolean: v = r.booleanValue ? "1" : "0"; break;
            case senml::ValueKind::Data:    v = r.dataValue; break;
            case senml::ValueKind::None:    break;
        }
        d.m_ridvalue.assign(v.begin(), v.end());
        d.m_ridlength = static_cast<std::uint32_t>(v.size());
        out.m_value.push_back(std::move(d));
    }
}

} // namespace

int SenmlJsonCodec::decode(const std::string& payload, LwM2MObject& out) {
    std::vector<senml::Record> recs;
    int rc = senml::decode_json(payload, recs);
    if (rc != 0) return rc;
    senml_to_lwm2m(recs, out);
    return 0;
}

int SenmlJsonCodec::encode(const LwM2MObject& /*in*/, std::string& /*payload*/) {
    // Encoding LwM2MObject → SenML is lossy without type info; callers
    // that need it go through ::lwm2m::senml::encode_json directly.
    return 0;
}

int SenmlCborCodec::decode(const std::string& payload, LwM2MObject& out) {
    std::vector<senml::Record> recs;
    int rc = senml::decode_cbor(payload, recs);
    if (rc != 0) return rc;
    senml_to_lwm2m(recs, out);
    return 0;
}

int SenmlCborCodec::encode(const LwM2MObject& /*in*/, std::string& /*payload*/) {
    return 0;
}

CodecRegistry& CodecRegistry::instance() {
    static CodecRegistry inst;
    return inst;
}

void CodecRegistry::add(std::shared_ptr<ICodec> codec) {
    if (!codec) return;
    m_codecs[static_cast<std::uint16_t>(codec->format())] = std::move(codec);
}

ICodec* CodecRegistry::find(ContentFormat cf) {
    auto it = m_codecs.find(static_cast<std::uint16_t>(cf));
    return it == m_codecs.end() ? nullptr : it->second.get();
}

namespace {
struct DefaultRegistrar {
    DefaultRegistrar() {
        CodecRegistry::instance().add(std::make_shared<TlvCodec>());
        CodecRegistry::instance().add(std::make_shared<SenmlJsonCodec>());
        CodecRegistry::instance().add(std::make_shared<SenmlCborCodec>());
    }
};

// Construct-on-load: registers TLV + SenML the first time this
// translation unit is touched.
DefaultRegistrar s_registrar;
} // namespace

} // namespace lwm2m
