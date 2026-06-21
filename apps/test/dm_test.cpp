#include <gtest/gtest.h>

#include "coap_adapter.hpp"
#include "lwm2m_codec_linkformat.hpp"
#include "lwm2m_codec_plaintext.hpp"
#include "lwm2m_codec_registry.hpp"      // CF_PlainText
#include "lwm2m_codec_tlv.hpp"
#include "lwm2m_cert_chunk.hpp"
#include "lwm2m_dm_client.hpp"
#include "lwm2m_dm_server.hpp"
#include "lwm2m_object_cert.hpp"
#include "lwm2m_object_store.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sys/stat.h>

using ::lwm2m::DmClient;
using ::lwm2m::DmOutcome;
using ::lwm2m::ObjectDescriptor;
using ::lwm2m::ObjectInstance;
using ::lwm2m::ObjectStore;
using ::lwm2m::Operations;
using ::lwm2m::Resource;
using ::lwm2m::ResourceType;
namespace dmsrv = ::lwm2m::dmsrv;

namespace {

std::shared_ptr<ObjectStore> make_device_store(std::string& manufacturer,
                                               std::string& deviceType,
                                               bool& rebootRan) {
    auto store = std::make_shared<ObjectStore>();
    ObjectDescriptor desc;
    desc.oid = 3; desc.name = "Device"; desc.urn = "urn:oma:lwm2m:oma:3:1.1";
    desc.mandatory = true;

    ObjectInstance inst; inst.iid = 0;

    Resource man;
    man.rid = 0; man.name = "Manufacturer"; man.type = ResourceType::String;
    man.ops = Operations::R;
    man.read = [&]() { return manufacturer; };
    inst.resources[0] = man;

    Resource dt;
    dt.rid = 17; dt.name = "Device Type"; dt.type = ResourceType::String;
    dt.ops = Operations::RW;
    dt.read  = [&]() { return deviceType; };
    dt.write = [&](const std::string& v) { deviceType = v; return 0; };
    inst.resources[17] = dt;

    Resource reb;
    reb.rid = 4; reb.name = "Reboot"; reb.type = ResourceType::None;
    reb.ops = Operations::E;
    reb.execute = [&](const std::string&) { rebootRan = true; return 0; };
    inst.resources[4] = reb;

    Resource curT;
    curT.rid = 13; curT.name = "Current Time"; curT.type = ResourceType::Time;
    curT.ops = Operations::R;
    curT.observable = true;
    curT.read = [&]() { return std::string("1700000000"); };
    inst.resources[13] = curT;

    desc.instances[0] = inst;
    store->add_object(desc);
    return store;
}

std::uint8_t code_of(const std::string& bytes) {
    return bytes.size() >= 2 ? static_cast<std::uint8_t>(bytes[1]) : 0;
}

} // namespace

/* ─────────────────────────── REQ-DM-001 Read ──────────────────────────── */

TEST(DmClient, REQ_DM_001_read_single_resource_plain_text) {
    std::string mfg = "Acme"; std::string dt = "sensor"; bool reb = false;
    auto store = make_device_store(mfg, dt, reb);
    DmClient dm(store);
    CoAPAdapter coap;

    // GET /3/0/0 — Accept defaults to plain text.
    auto req = dmsrv::build_read(0x100, std::string{0x01},
                                 /*oid*/3, /*iid*/0, /*rid*/0, /*accept*/-1);
    CoAPAdapter::CoAPMessage parsed;
    coap.parseRequest(req, parsed);
    auto out = dm.handle(parsed, coap);
    EXPECT_EQ(DmOutcome::Read, out.kind);
    EXPECT_EQ(0x45, code_of(out.response));   // 2.05 Content
    // Payload "Acme" should appear after the 0xFF marker.
    auto pos = out.response.find(static_cast<char>(0xFF));
    ASSERT_NE(std::string::npos, pos);
    EXPECT_EQ("Acme", out.response.substr(pos + 1));
}

TEST(DmClient, REQ_DM_001_read_unknown_resource_404) {
    std::string m,d; bool r=false;
    auto store = make_device_store(m, d, r);
    DmClient dm(store);
    CoAPAdapter coap;

    auto req = dmsrv::build_read(1, std::string{0x01}, 3, 0, /*rid=*/99, -1);
    CoAPAdapter::CoAPMessage parsed;  coap.parseRequest(req, parsed);
    auto out = dm.handle(parsed, coap);
    EXPECT_EQ(DmOutcome::Error, out.kind);
    EXPECT_EQ(0x84, code_of(out.response));
}

TEST(DmClient, REQ_DM_001_read_write_only_resource_405) {
    auto store = std::make_shared<ObjectStore>();
    ObjectDescriptor d; d.oid = 100;
    ObjectInstance i; i.iid = 0;
    Resource w; w.rid = 0; w.type = ResourceType::String; w.ops = Operations::W;
    w.write = [](const std::string&) { return 0; };
    i.resources[0] = w; d.instances[0] = i;
    store->add_object(d);
    DmClient dm(store);
    CoAPAdapter coap;

    auto req = dmsrv::build_read(1, std::string{0x01}, 100, 0, 0, -1);
    CoAPAdapter::CoAPMessage parsed;  coap.parseRequest(req, parsed);
    EXPECT_EQ(0x85, code_of(dm.handle(parsed, coap).response));   // 4.05
}

/* ─────────────────────────── REQ-DM-002 Discover ──────────────────────── */

TEST(DmClient, REQ_DM_002_discover_returns_link_format) {
    std::string m,d; bool r=false;
    auto store = make_device_store(m, d, r);
    DmClient dm(store);
    CoAPAdapter coap;

    auto req = dmsrv::build_discover(1, std::string{0x01}, 3, -1, -1);
    CoAPAdapter::CoAPMessage parsed; coap.parseRequest(req, parsed);
    auto out = dm.handle(parsed, coap);
    EXPECT_EQ(DmOutcome::Discover, out.kind);
    EXPECT_EQ(0x45, code_of(out.response));
    auto pos = out.response.find(static_cast<char>(0xFF));
    ASSERT_NE(std::string::npos, pos);
    std::string body = out.response.substr(pos + 1);
    EXPECT_NE(std::string::npos, body.find("</3>"));
    EXPECT_NE(std::string::npos, body.find("</3/0>"));
}

/* ─────────────────────────── REQ-DM-003 Write ─────────────────────────── */

TEST(DmClient, REQ_DM_003_PUT_writes_single_resource_plain_text) {
    std::string m="Acme", d="sensor"; bool r=false;
    auto store = make_device_store(m, d, r);
    DmClient dm(store);
    CoAPAdapter coap;

    auto req = dmsrv::build_write(1, std::string{0x01}, 3, 0, 17,
                                  /*cf*/ ::lwm2m::CF_PlainText,
                                  /*payload*/ "actuator",
                                  /*partial*/ false);
    CoAPAdapter::CoAPMessage parsed; coap.parseRequest(req, parsed);
    auto out = dm.handle(parsed, coap);
    EXPECT_EQ(DmOutcome::Write, out.kind);
    EXPECT_EQ(0x44, code_of(out.response));   // 2.04
    EXPECT_EQ("actuator", d);
}

TEST(DmClient, REQ_DM_003_PUT_to_read_only_405) {
    std::string m="Acme", d=""; bool r=false;
    auto store = make_device_store(m, d, r);
    DmClient dm(store);
    CoAPAdapter coap;

    auto req = dmsrv::build_write(1, std::string{0x01}, 3, 0, /*rid=*/0,
                                  ::lwm2m::CF_PlainText, "Other", false);
    CoAPAdapter::CoAPMessage parsed; coap.parseRequest(req, parsed);
    auto out = dm.handle(parsed, coap);
    EXPECT_EQ(0x85, code_of(out.response));
    EXPECT_EQ("Acme", m);   // unchanged
}

TEST(DmClient, REQ_DM_003_unsupported_cf_415) {
    std::string m="Acme", d="sensor"; bool r=false;
    auto store = make_device_store(m, d, r);
    DmClient dm(store);
    CoAPAdapter coap;

    auto req = dmsrv::build_write(1, std::string{0x01}, 3, 0, 17,
                                  /*cf=*/ 9999, /*payload=*/ "x", false);
    CoAPAdapter::CoAPMessage parsed; coap.parseRequest(req, parsed);
    EXPECT_EQ(0x8F, code_of(dm.handle(parsed, coap).response));
}

/* ─────────────────────────── REQ-DM-005 Delete ────────────────────────── */

TEST(DmClient, REQ_DM_005_DELETE_removes_instance) {
    auto store = std::make_shared<ObjectStore>();
    ObjectDescriptor d;  d.oid = 10; d.multipleInstance = true;
    ObjectInstance i;  i.iid = 1; d.instances[1] = i;
    store->add_object(d);
    DmClient dm(store);
    CoAPAdapter coap;

    auto req = dmsrv::build_delete(1, std::string{0x01}, 10, 1);
    CoAPAdapter::CoAPMessage parsed; coap.parseRequest(req, parsed);
    auto out = dm.handle(parsed, coap);
    EXPECT_EQ(DmOutcome::Delete, out.kind);
    EXPECT_EQ(0x42, code_of(out.response));   // 2.02
    EXPECT_FALSE(store->has(10, 1));
}

/* ─────────────────────────── REQ-DM-006 Execute ───────────────────────── */

TEST(DmClient, REQ_DM_006_POST_to_executable_fires_callback) {
    std::string m, d; bool reb = false;
    auto store = make_device_store(m, d, reb);
    DmClient dm(store);
    CoAPAdapter coap;

    auto req = dmsrv::build_execute(1, std::string{0x01}, 3, 0, 4, /*args*/ {});
    CoAPAdapter::CoAPMessage parsed; coap.parseRequest(req, parsed);
    auto out = dm.handle(parsed, coap);
    EXPECT_EQ(DmOutcome::Execute, out.kind);
    EXPECT_EQ(0x44, code_of(out.response));
    EXPECT_TRUE(reb);
}

TEST(DmClient, REQ_DM_006_POST_to_non_executable_405) {
    std::string m="Acme", d="x"; bool reb = false;
    auto store = make_device_store(m, d, reb);
    DmClient dm(store);
    CoAPAdapter coap;

    auto req = dmsrv::build_execute(1, std::string{0x01}, 3, 0, /*rid=*/0, {});
    CoAPAdapter::CoAPMessage parsed; coap.parseRequest(req, parsed);
    EXPECT_EQ(0x85, code_of(dm.handle(parsed, coap).response));
}

/* ─────────────────────────── REQ-DM-007 Write-Attributes ─────────────── */

TEST(DmClient, REQ_DM_007_PUT_with_pmin_pmax_updates_attrs_per_ssid) {
    std::string m, d; bool reb = false;
    auto store = make_device_store(m, d, reb);
    DmClient dm(store);
    dm.calling_short_server_id(7);
    CoAPAdapter coap;

    std::uint32_t pmin = 5, pmax = 60;
    dmsrv::AttributeUpdate u;  u.pmin = &pmin;  u.pmax = &pmax;
    auto req = dmsrv::build_write_attributes(1, std::string{0x01},
                                             3, 0, 13, u);
    CoAPAdapter::CoAPMessage parsed; coap.parseRequest(req, parsed);
    auto out = dm.handle(parsed, coap);
    EXPECT_EQ(DmOutcome::WriteAttributes, out.kind);
    EXPECT_EQ(0x44, code_of(out.response));

    auto* r = store->find(3, 0, 13);
    ASSERT_NE(nullptr, r);
    ASSERT_EQ(1u, r->attrs.size());
    EXPECT_EQ(7u, r->attrs[0].shortServerId);
    EXPECT_EQ(5u, r->attrs[0].pmin);
    EXPECT_EQ(60u, r->attrs[0].pmax);
}

/* ─────────────────────────── REQ-DM-004 Create ────────────────────────── */

TEST(DmClient, REQ_DM_004_POST_oid_creates_new_instance) {
    auto store = std::make_shared<ObjectStore>();
    ObjectDescriptor d;  d.oid = 20; d.multipleInstance = true;
    Resource tmpl;
    tmpl.rid  = 0;  tmpl.type = ResourceType::String;
    tmpl.ops  = Operations::RW;
    // No live binding — templates are copied per-instance and the
    // instance-level Resource provides its own callbacks. For this test
    // we attach a sink that the write path can run without crashing.
    static std::string sink;
    tmpl.read  = []() { return sink; };
    tmpl.write = [](const std::string& v) { sink = v; return 0; };
    d.resourceTemplates[0] = tmpl;
    store->add_object(d);

    DmClient dm(store);
    CoAPAdapter coap;

    // Construct a TLV container with rid=0 value="hello".
    std::string body;
    ::lwm2m::tlv::encode_string(TypeBits76_ResourceWithValue_11, "hello", 0, body);
    auto req = dmsrv::build_create(1, std::string{0x01}, 20, body);
    CoAPAdapter::CoAPMessage parsed; coap.parseRequest(req, parsed);

    auto out = dm.handle(parsed, coap);
    EXPECT_EQ(DmOutcome::Create, out.kind);
    EXPECT_EQ(0x41, code_of(out.response));   // 2.01 Created
    EXPECT_TRUE(store->has(20, 0));
    EXPECT_EQ("hello", sink);
}

TEST(DmClient, REQ_DM_004_POST_to_single_instance_object_405) {
    auto store = std::make_shared<ObjectStore>();
    ObjectDescriptor d;  d.oid = 21; d.multipleInstance = false;
    store->add_object(d);
    DmClient dm(store);
    CoAPAdapter coap;

    auto req = dmsrv::build_create(1, std::string{0x01}, 21, std::string{});
    CoAPAdapter::CoAPMessage parsed; coap.parseRequest(req, parsed);
    EXPECT_EQ(0x85, code_of(dm.handle(parsed, coap).response));
}

/* ─────────────────────────── REQ-ENC-005 / 006 plain text + opaque ───── */

TEST(Plaintext, REQ_ENC_005_decode_boolean) {
    std::string out;
    EXPECT_EQ(0, ::lwm2m::plaintext::decode(ResourceType::Boolean, "1", out));
    EXPECT_EQ("1", out);
    EXPECT_EQ(0, ::lwm2m::plaintext::decode(ResourceType::Boolean, "0", out));
    EXPECT_EQ("0", out);
    EXPECT_EQ(-1, ::lwm2m::plaintext::decode(ResourceType::Boolean, "true", out));
    EXPECT_EQ(-1, ::lwm2m::plaintext::decode(ResourceType::Boolean, "yes", out));
}

TEST(Plaintext, REQ_ENC_005_decode_integer_rejects_garbage) {
    std::string out;
    EXPECT_EQ(0, ::lwm2m::plaintext::decode(ResourceType::Integer, "-42", out));
    EXPECT_EQ("-42", out);
    EXPECT_EQ(-1, ::lwm2m::plaintext::decode(ResourceType::Integer, "12a", out));
    EXPECT_EQ(-1, ::lwm2m::plaintext::decode(ResourceType::Integer, "", out));
}

TEST(Plaintext, REQ_ENC_005_decode_float_accepts_exponent) {
    std::string out;
    EXPECT_EQ(0, ::lwm2m::plaintext::decode(ResourceType::Float, "1.5e3", out));
    EXPECT_EQ(-1, ::lwm2m::plaintext::decode(ResourceType::Float, "1.5e", out));
    EXPECT_EQ(-1, ::lwm2m::plaintext::decode(ResourceType::Float, ".e3", out));
}

/* ─────────────────────────── DM server builders ──────────────────────── */

TEST(DmServer, build_uri_matches_path_shape) {
    EXPECT_EQ("/3",      dmsrv::build_uri(3));
    EXPECT_EQ("/3/0",    dmsrv::build_uri(3, 0));
    EXPECT_EQ("/3/0/13", dmsrv::build_uri(3, 0, 13));
    EXPECT_EQ("/3/0/13/4", dmsrv::build_uri(3, 0, 13, 4));
}

TEST(DmServer, read_request_round_trips_through_parseRequest) {
    auto bytes = dmsrv::build_read(0xABCD, std::string{0x55},
                                   3, 0, 13, ::lwm2m::CF_PlainText);
    CoAPAdapter coap;
    CoAPAdapter::CoAPMessage parsed;
    coap.parseRequest(bytes, parsed);
    EXPECT_EQ(0xABCD, parsed.coapheader.msgid);
    EXPECT_EQ(1u, parsed.coapheader.code);    // GET
}

/* ───── Phase 3: server→device VPN cert push over Object 2048 (loopback) ── */

namespace {
std::string slurp_file(const std::string& p) {
    std::ifstream ifs(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
}
} // namespace

// Drives the exact bytes build_cert_push() emits through a device that has
// Object 2048 installed: three opaque WRITEs (CA/cert/key) then the Apply
// EXECUTE. Asserts each frame is accepted (2.04) and the family lands on disk.
TEST(CertPush, server_push_materializes_family_on_device) {
    char tmpl[] = "/tmp/iot-certpush-XXXXXX";
    char* dir = ::mkdtemp(tmpl);
    if (!dir) GTEST_SKIP() << "no writable scratch dir";
    const std::string certDir(dir);

    auto store = std::make_shared<ObjectStore>();
    ::lwm2m::objects::install_cert(*store, certDir);
    DmClient dm(store);
    CoAPAdapter coap;

    const std::string ca   = "-----BEGIN CERTIFICATE-----\nCA\n-----END CERTIFICATE-----\n";
    const std::string cert = "-----BEGIN CERTIFICATE-----\nCLIENT\n-----END CERTIFICATE-----\n";
    const std::string key  = "-----BEGIN PRIVATE KEY-----\nKEY\n-----END PRIVATE KEY-----\n";

    std::uint16_t mid = 0x2000;
    auto frames = dmsrv::build_cert_push([&mid] { return mid++; },
                                         std::string{static_cast<char>(0x04)},
                                         ca, cert, key);
    // Small PEMs → 1 chunk each → 3 WRITEs + 1 EXECUTE.
    ASSERT_EQ(4u, frames.size());

    // First three WRITEs stage; nothing on disk yet.
    for (std::size_t i = 0; i < 3; ++i) {
        CoAPAdapter::CoAPMessage parsed; coap.parseRequest(frames[i], parsed);
        auto out = dm.handle(parsed, coap);
        EXPECT_EQ(DmOutcome::Write, out.kind) << "frame " << i;
        EXPECT_EQ(0x44, code_of(out.response)) << "frame " << i;   // 2.04
    }
    struct stat st;
    EXPECT_NE(0, ::stat((certDir + "/ca.crt").c_str(), &st));      // not yet

    // Apply EXECUTE commits the family.
    CoAPAdapter::CoAPMessage applyMsg; coap.parseRequest(frames[3], applyMsg);
    auto out = dm.handle(applyMsg, coap);
    EXPECT_EQ(DmOutcome::Execute, out.kind);
    EXPECT_EQ(0x44, code_of(out.response));                        // 2.04

    EXPECT_EQ(ca,   slurp_file(certDir + "/ca.crt"));
    EXPECT_EQ(cert, slurp_file(certDir + "/client.crt"));
    EXPECT_EQ(key,  slurp_file(certDir + "/client.key"));
    ASSERT_EQ(0, ::stat((certDir + "/client.key").c_str(), &st));
    EXPECT_EQ(0640, st.st_mode & 0777);            // key 0640: group-readable by design

    for (auto* f : {"/ca.crt", "/client.crt", "/client.key"})
        std::remove((certDir + f).c_str());
    std::remove(certDir.c_str());
}

/* ─────────────── zip + chunk codec for large cert payloads ─────────────── */

TEST(CertChunk, small_payload_single_unzipped_chunk) {
    auto frames = ::lwm2m::certchunk::encode("hello", /*maxChunkData*/ 1019);
    ASSERT_EQ(1u, frames.size());
    EXPECT_EQ(0, frames[0][0] & 1);        // not zipped
    ::lwm2m::certchunk::Reassembler r;
    std::string out;
    EXPECT_EQ(1, r.feed(frames[0], out));
    EXPECT_EQ("hello", out);
}

TEST(CertChunk, large_payload_zipped_and_chunked_roundtrips) {
    // ~4 KB of PEM-ish text (compresses, then exceeds one chunk).
    std::string big;
    for (int i = 0; i < 120; ++i)
        big += "-----CERT LINE " + std::to_string(i) + " AAAAAAAAAAAAAAAAAAAA-----\n";
    ASSERT_GT(big.size(), 1024u);

    auto frames = ::lwm2m::certchunk::encode(big, /*maxChunkData*/ 200);
    ASSERT_GT(frames.size(), 1u);          // multiple chunks
    EXPECT_EQ(1, frames[0][0] & 1);        // zipped
    for (auto& f : frames) EXPECT_LE(f.size(), 200u + ::lwm2m::certchunk::kHeader);

    // Reassemble in order.
    ::lwm2m::certchunk::Reassembler r;
    std::string out; int rc = 0;
    for (auto& f : frames) { rc = r.feed(f, out); }
    EXPECT_EQ(1, rc);
    EXPECT_EQ(big, out);

    // Out-of-order + duplicate delivery still reassembles.
    ::lwm2m::certchunk::Reassembler r2;
    std::string out2; rc = 0;
    rc = r2.feed(frames.back(), out2);     EXPECT_EQ(0, rc);
    rc = r2.feed(frames.back(), out2);     EXPECT_EQ(0, rc);   // dup ignored
    for (std::size_t i = 0; i + 1 < frames.size(); ++i) rc = r2.feed(frames[i], out2);
    EXPECT_EQ(1, rc);
    EXPECT_EQ(big, out2);
}

TEST(CertChunk, malformed_chunk_rejected) {
    ::lwm2m::certchunk::Reassembler r;
    std::string out;
    EXPECT_EQ(-1, r.feed("xy", out));       // shorter than the header
}

// Server-side confirmation: after the device applies a family, the DM server
// READs /2048/0/4 (status) and must CONSUME the device's 2.05 response — the
// payload (applied fingerprint) routed to the dmResponseHandler, the token
// (endpoint id) preserved, and NOT dispatched as a request.
TEST(CertConfirm, server_consumes_status_read_response) {
    char tmpl[] = "/tmp/iot-confirm-XXXXXX";
    char* dir = ::mkdtemp(tmpl);
    if (!dir) GTEST_SKIP() << "no writable scratch dir";
    const std::string certDir(dir);

    auto store = std::make_shared<ObjectStore>();
    ::lwm2m::objects::install_cert(*store, certDir);
    // RID 1 now carries chunk-framed payloads — feed via the codec.
    auto feed = [&](std::uint32_t iid, const std::string& pem) {
        for (auto& c : ::lwm2m::certchunk::encode(pem))
            store->find(2048, iid, 1)->write(c);
    };
    feed(0, "CA"); feed(1, "CERT"); feed(2, "KEY");
    ASSERT_EQ(0, store->find(2048, 0, 3)->execute(""));
    const std::string fp = store->find(2048, 0, 4)->read();
    ASSERT_FALSE(fp.empty());

    // Device answers the server's status Read.
    DmClient dm(store);
    CoAPAdapter devCoap;
    std::string tok;                       // {0x05, idHi=0x00, idLo=0x07}
    tok.push_back(static_cast<char>(0x05));
    tok.push_back(static_cast<char>(0x00));
    tok.push_back(static_cast<char>(0x07));
    auto rd = dmsrv::build_read(0x321, tok, 2048, 0, 4, /*accept*/ -1);
    CoAPAdapter::CoAPMessage parsed; devCoap.parseRequest(rd, parsed);
    auto out = dm.handle(parsed, devCoap);
    ASSERT_EQ(DmOutcome::Read, out.kind);

    // Server consumes the response via the handler (not the request path).
    CoAPAdapter srv;
    std::string gotPayload; std::vector<std::uint8_t> gotTok;
    srv.dmResponseHandler([&](const CoAPAdapter::CoAPMessage& m) {
        gotPayload = m.payload; gotTok = m.tokens;
    });
    std::vector<std::string> sout;
    srv.processRequest(/*isAmIClient*/ false, out.response, sout);

    EXPECT_EQ(fp, gotPayload);              // device's applied fingerprint
    ASSERT_GE(gotTok.size(), 3u);
    EXPECT_EQ(0x05, gotTok[0]);             // our status-token marker
    EXPECT_EQ(0x07, gotTok[2]);             // endpoint id echoed back
    EXPECT_TRUE(sout.empty());             // consumed, not dispatched as request

    for (auto* f : {"/ca.crt", "/client.crt", "/client.key"})
        std::remove((certDir + f).c_str());
    std::remove(certDir.c_str());
}
