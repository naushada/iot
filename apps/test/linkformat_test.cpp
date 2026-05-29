#include <gtest/gtest.h>

#include "lwm2m_codec_linkformat.hpp"
#include "lwm2m_object_store.hpp"

using ::lwm2m::ObjectDescriptor;
using ::lwm2m::ObjectInstance;
using ::lwm2m::ObjectStore;
using ::lwm2m::Operations;
using ::lwm2m::Resource;
using ::lwm2m::ResourceType;
using ::lwm2m::NotificationAttributes;

namespace lf = ::lwm2m::linkformat;

namespace {

ObjectStore make_minimal_store() {
    ObjectStore s;

    ObjectDescriptor server;
    server.oid  = 1;
    server.name = "Server";
    server.urn  = "urn:oma:lwm2m:oma:1:1.1";
    ObjectInstance si;
    si.iid = 0;
    server.instances[0] = si;
    s.add_object(server);

    ObjectDescriptor device;
    device.oid  = 3;
    device.name = "Device";
    device.urn  = "urn:oma:lwm2m:oma:3:1.1";
    ObjectInstance di;
    di.iid = 0;
    Resource time;
    time.rid        = 13;
    time.type       = ResourceType::Time;
    time.ops        = Operations::R;
    time.observable = true;
    di.resources[13] = time;
    device.instances[0] = di;
    s.add_object(device);

    return s;
}

} // namespace

/* ─────────────────────────── REQ-REG-008 root entry ──────────────────── */

TEST(LinkFormat, REQ_REG_008_register_payload_starts_with_root_entry) {
    auto store = make_minimal_store();
    auto entries = lf::register_payload(store);
    ASSERT_FALSE(entries.empty());

    EXPECT_EQ("/", entries.front().uri);
    auto* rt = entries.front().find("rt");
    ASSERT_NE(nullptr, rt);
    EXPECT_EQ("oma.lwm2m", rt->value);
    EXPECT_TRUE(rt->quoted);

    auto* ct = entries.front().find("ct");
    ASSERT_NE(nullptr, ct);
    EXPECT_EQ("11542", ct->value);
    EXPECT_FALSE(ct->quoted);
}

TEST(LinkFormat, REQ_REG_008_register_payload_lists_all_instances) {
    auto store = make_minimal_store();
    auto entries = lf::register_payload(store);

    // root + /1/0 + /3/0 (ver attribute on first /1/0 since v1.1)
    ASSERT_EQ(3u, entries.size());
    EXPECT_EQ("/",   entries[0].uri);
    EXPECT_EQ("/1/0", entries[1].uri);
    EXPECT_EQ("/3/0", entries[2].uri);

    auto* ver = entries[1].find("ver");
    ASSERT_NE(nullptr, ver);
    EXPECT_EQ("1.1", ver->value);
    EXPECT_TRUE(ver->quoted);
}

TEST(LinkFormat, REQ_REG_008_register_payload_skips_object_with_no_instance) {
    ObjectStore s;
    ObjectDescriptor d;
    d.oid = 5;
    d.name = "FirmwareUpdate";
    d.urn  = "urn:oma:lwm2m:oma:5:1.1";
    // no instances added
    s.add_object(d);

    auto entries = lf::register_payload(s);
    ASSERT_EQ(1u, entries.size());   // just the root
    EXPECT_EQ("/", entries.front().uri);
}

TEST(LinkFormat, REQ_REG_008_register_payload_encoded_shape) {
    auto store = make_minimal_store();
    auto text  = lf::encode(lf::register_payload(store));
    EXPECT_EQ("</>;rt=\"oma.lwm2m\";ct=11542,"
              "</1/0>;ver=\"1.1\","
              "</3/0>",
              text);
}

/* ─────────────────────────── REQ-ENC-007 roundtrip ───────────────────── */

TEST(LinkFormat, REQ_ENC_007_roundtrip_simple) {
    std::vector<lf::LinkEntry> in;
    lf::LinkEntry a;  a.uri = "/3/0/13"; a.set("pmin", 5u); a.set("pmax", 30u);
    lf::LinkEntry b;  b.uri = "/1/0";
    in.push_back(a);
    in.push_back(b);

    auto text = lf::encode(in);

    std::vector<lf::LinkEntry> out;
    ASSERT_EQ(0, lf::decode(text, out));
    ASSERT_EQ(2u, out.size());

    EXPECT_EQ("/3/0/13", out[0].uri);
    EXPECT_EQ("/1/0",    out[1].uri);

    auto* pmin = out[0].find("pmin");
    auto* pmax = out[0].find("pmax");
    ASSERT_NE(nullptr, pmin);  EXPECT_EQ("5",  pmin->value);
    ASSERT_NE(nullptr, pmax);  EXPECT_EQ("30", pmax->value);
}

TEST(LinkFormat, REQ_ENC_007_roundtrip_quoted_string) {
    std::vector<lf::LinkEntry> in;
    lf::LinkEntry e;
    e.uri = "/";
    e.set("rt", "oma.lwm2m core", /*force_quote*/ true);   // has a space → quoted
    e.set("title", "Hello, \"World\"", /*force_quote*/ true);
    in.push_back(e);

    auto text = lf::encode(in);

    std::vector<lf::LinkEntry> out;
    ASSERT_EQ(0, lf::decode(text, out));
    ASSERT_EQ(1u, out.size());
    EXPECT_EQ("/", out[0].uri);
    auto* rt = out[0].find("rt");
    ASSERT_NE(nullptr, rt);
    EXPECT_EQ("oma.lwm2m core", rt->value);
    EXPECT_TRUE(rt->quoted);
    auto* title = out[0].find("title");
    ASSERT_NE(nullptr, title);
    EXPECT_EQ("Hello, \"World\"", title->value);
}

TEST(LinkFormat, REQ_ENC_007_roundtrip_flag_attribute) {
    std::vector<lf::LinkEntry> in;
    lf::LinkEntry e;
    e.uri = "/3/0/13";
    e.set_flag("obs");
    in.push_back(e);

    auto text = lf::encode(in);
    EXPECT_EQ("</3/0/13>;obs", text);

    std::vector<lf::LinkEntry> out;
    ASSERT_EQ(0, lf::decode(text, out));
    ASSERT_EQ(1u, out.size());
    auto* obs = out[0].find("obs");
    ASSERT_NE(nullptr, obs);
    EXPECT_TRUE(obs->value.empty());
}

TEST(LinkFormat, REQ_ENC_007_decode_malformed_unbalanced_uri_brackets) {
    std::vector<lf::LinkEntry> out;
    EXPECT_EQ(-1, lf::decode("</3/0/13;obs", out));
}

TEST(LinkFormat, REQ_ENC_007_decode_malformed_unterminated_quote) {
    std::vector<lf::LinkEntry> out;
    EXPECT_EQ(-1, lf::decode("</>;rt=\"oma", out));
}

TEST(LinkFormat, REQ_ENC_007_decode_empty_input) {
    std::vector<lf::LinkEntry> out;
    EXPECT_EQ(0, lf::decode("", out));
    EXPECT_EQ(0u, out.size());
}

TEST(LinkFormat, REQ_ENC_007_decode_handles_whitespace) {
    std::vector<lf::LinkEntry> out;
    EXPECT_EQ(0, lf::decode(" </1/0> , </3/0> ", out));
    ASSERT_EQ(2u, out.size());
    EXPECT_EQ("/1/0", out[0].uri);
    EXPECT_EQ("/3/0", out[1].uri);
}

/* ─────────────────────────── REQ-DM-002 / discover ───────────────────── */

TEST(LinkFormat, REQ_DM_002_discover_whole_object) {
    auto store = make_minimal_store();
    auto entries = lf::discover(store, /*oid*/ 3, /*iid*/ -1, /*rid*/ -1,
                                /*shortServerId*/ 1);
    // Expect: object root + instance + resource 13
    ASSERT_EQ(3u, entries.size());
    EXPECT_EQ("/3",      entries[0].uri);
    EXPECT_EQ("/3/0",    entries[1].uri);
    EXPECT_EQ("/3/0/13", entries[2].uri);

    auto* ver = entries[0].find("ver");
    ASSERT_NE(nullptr, ver);
    EXPECT_EQ("1.1", ver->value);
}

TEST(LinkFormat, REQ_DM_002_discover_single_resource_observable) {
    auto store = make_minimal_store();
    auto entries = lf::discover(store, 3, 0, 13, /*shortServerId*/ 1);
    ASSERT_EQ(1u, entries.size());
    EXPECT_EQ("/3/0/13", entries[0].uri);
    EXPECT_NE(nullptr, entries[0].find("obs"));   // observable resource
}

TEST(LinkFormat, REQ_DM_002_discover_resource_with_attributes_keyed_by_ssid) {
    // Decision D2: attributes are keyed by Short Server ID. Two server-side
    // attribute rows on the same resource; Discover must select the matching
    // one and ignore the other.
    auto store = make_minimal_store();
    auto* r = store.find(3, 0, 13);
    ASSERT_NE(nullptr, r);

    NotificationAttributes ssid1;
    ssid1.shortServerId = 1;
    ssid1.pmin = 5;  ssid1.pmax = 30;
    r->attrs.push_back(ssid1);

    NotificationAttributes ssid7;
    ssid7.shortServerId = 7;
    ssid7.pmin = 60; ssid7.pmax = 600;
    r->attrs.push_back(ssid7);

    auto entries = lf::discover(store, 3, 0, 13, /*shortServerId*/ 7);
    ASSERT_EQ(1u, entries.size());
    auto* pmin = entries[0].find("pmin");
    auto* pmax = entries[0].find("pmax");
    ASSERT_NE(nullptr, pmin);  EXPECT_EQ("60",  pmin->value);
    ASSERT_NE(nullptr, pmax);  EXPECT_EQ("600", pmax->value);
}

TEST(LinkFormat, REQ_DM_002_discover_unknown_uri_returns_empty) {
    auto store = make_minimal_store();
    EXPECT_TRUE(lf::discover(store, 99, -1, -1, 1).empty());
    EXPECT_TRUE(lf::discover(store, 3, 99, -1, 1).empty());
    EXPECT_TRUE(lf::discover(store, 3, 0, 99, 1).empty());
}
