/// D1 risk-gate tests for the data-store module.

#include <gtest/gtest.h>

#include "data_store/proto.hpp"
#include "data_store/value.hpp"
#include "../src/server/data_store.hpp"

using data_store::server::DataStore;
using data_store::Value;
namespace proto = data_store::proto;

/* ───────────────────────────── proto ──────────────────────────────── */

TEST(Proto, parse_op_round_trips_known_opcodes) {
    EXPECT_EQ(proto::Op::Set,           proto::parse_op(0x0001));
    EXPECT_EQ(proto::Op::Get,           proto::parse_op(0x0002));
    EXPECT_EQ(proto::Op::RegisterWatch, proto::parse_op(0x0003));
    EXPECT_EQ(proto::Op::RemoveWatch,   proto::parse_op(0x0004));
    EXPECT_EQ(proto::Op::NotifyEvent,   proto::parse_op(0x0064));
    EXPECT_EQ(proto::Op::Unknown,       proto::parse_op(0x9999));
    EXPECT_EQ(proto::Op::Unknown,       proto::parse_op(0));
}

TEST(Proto, encode_decode_header_round_trip) {
    proto::Header in;
    in.cmdID        = 0x1234;
    in.type         = proto::Response;
    in.reqID        = 42;
    in.payload_size = 1024;
    char buf[proto::kHeaderSize];
    proto::encode_header(in, buf);
    proto::Header out;
    proto::decode_header(buf, out);
    EXPECT_EQ(in.cmdID,        out.cmdID);
    EXPECT_EQ(in.type,         out.type);
    EXPECT_EQ(in.reqID,        out.reqID);
    EXPECT_EQ(in.payload_size, out.payload_size);
}

TEST(Proto, encode_command_then_decode_frame) {
    std::string wire;
    proto::encode_frame_command(proto::Op::Get, 7, R"({"keys":["a"]})", wire);
    proto::Header h;
    std::string body;
    ASSERT_TRUE(proto::try_decode_frame(wire, h, body));
    EXPECT_EQ(static_cast<std::uint16_t>(proto::Op::Get), h.cmdID);
    EXPECT_EQ(0u, h.type);
    EXPECT_EQ(7u, h.reqID);
    EXPECT_EQ(R"({"keys":["a"]})", body);
    EXPECT_TRUE(wire.empty());
}

TEST(Proto, encode_response_carries_status_prefix) {
    std::string wire;
    proto::encode_frame_response(proto::Op::Set, 3, proto::Status::Ok,
                                 std::string_view{}, wire);
    proto::Header h;
    std::string body;
    ASSERT_TRUE(proto::try_decode_frame(wire, h, body));
    EXPECT_EQ(proto::Response, h.type);
    ASSERT_GE(body.size(), 2u);
    EXPECT_EQ(0, static_cast<std::uint8_t>(body[0]));
    EXPECT_EQ(0, static_cast<std::uint8_t>(body[1]));
}

TEST(Proto, encode_push_has_push_bit_and_no_status_prefix) {
    std::string wire;
    proto::encode_frame_push(proto::Op::NotifyEvent,
                             R"({"k":"foo","v":1})", wire);
    proto::Header h;
    std::string body;
    ASSERT_TRUE(proto::try_decode_frame(wire, h, body));
    EXPECT_TRUE(proto::is_push(h.type));
    EXPECT_FALSE(proto::is_response(h.type));
    EXPECT_EQ(0u, h.reqID);
    EXPECT_EQ(R"({"k":"foo","v":1})", body);
}

TEST(Proto, try_decode_returns_false_when_buffer_short) {
    std::string wire;
    proto::encode_frame_command(proto::Op::Set, 1, R"({"keys":[]})", wire);
    // Snip the last 3 bytes to simulate an incomplete recv.
    std::string truncated = wire.substr(0, wire.size() - 3);
    proto::Header h;
    std::string body;
    EXPECT_FALSE(proto::try_decode_frame(truncated, h, body));
    EXPECT_EQ(wire.size() - 3, truncated.size());
}

TEST(Proto, try_decode_rejects_oversized_payload) {
    // Hand-craft a header claiming payload_size > kMaxPayloadBytes.
    proto::Header h;
    h.cmdID        = 0x0001;
    h.type         = 0;
    h.reqID        = 1;
    h.payload_size = proto::kMaxPayloadBytes + 1;
    char buf[proto::kHeaderSize];
    proto::encode_header(h, buf);
    std::string wire(buf, proto::kHeaderSize);
    proto::Header out;
    std::string body;
    EXPECT_THROW(proto::try_decode_frame(wire, out, body),
                 std::runtime_error);
}

/* ───────────────────────────── store ──────────────────────────────── */

TEST(DataStore, empty_get_returns_nullopt) {
    DataStore s;
    EXPECT_EQ(0u, s.size());
    EXPECT_FALSE(s.get("missing").has_value());
}

TEST(DataStore, set_then_get_returns_value) {
    DataStore s;
    auto r = s.set("k", Value{std::string("v")});
    EXPECT_TRUE(r.changed);
    EXPECT_FALSE(r.prev.has_value());        // new key
    EXPECT_TRUE(r.watchers.empty());
    EXPECT_EQ(1u, s.size());
    auto v = s.get("k");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(std::string("v"), std::get<std::string>(*v));
}

TEST(DataStore, second_set_returns_previous) {
    DataStore s;
    s.set("k", Value{std::string("v1")});
    auto r = s.set("k", Value{std::string("v2")});
    EXPECT_TRUE(r.changed);
    ASSERT_TRUE(r.prev.has_value());
    EXPECT_EQ(std::string("v1"), std::get<std::string>(*r.prev));
    EXPECT_EQ(std::string("v2"), std::get<std::string>(*s.get("k")));
    EXPECT_EQ(1u, s.size());
}

TEST(DataStore, unchanged_set_reports_not_changed) {
    DataStore s;
    s.set("k", Value{std::string("v")});
    auto r = s.set("k", Value{std::string("v")});
    EXPECT_FALSE(r.changed);                 // REQ-DS-006
    EXPECT_FALSE(r.prev.has_value());        // no notify, no snapshot
    EXPECT_TRUE(r.watchers.empty());
}

TEST(DataStore, remove_returns_true_only_when_present) {
    DataStore s;
    s.set("k", Value{std::string("v")});
    EXPECT_TRUE(s.remove("k"));
    EXPECT_FALSE(s.remove("k"));
    EXPECT_FALSE(s.get("k").has_value());
}

TEST(DataStore, integer_value_round_trips) {
    DataStore s;
    auto r = s.set("counter", Value{static_cast<std::uint32_t>(42)});
    EXPECT_TRUE(r.changed);
    auto v = s.get("counter");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(42u, std::get<std::uint32_t>(*v));
}

TEST(DataStore, boolean_value_round_trips) {
    DataStore s;
    s.set("enabled", Value{true});
    auto v = s.get("enabled");
    ASSERT_TRUE(v.has_value());
    EXPECT_TRUE(std::get<bool>(*v));
}

TEST(DataStore, typed_unchanged_set_is_noop) {
    DataStore s;
    s.set("x", Value{static_cast<std::int32_t>(-7)});
    auto r = s.set("x", Value{static_cast<std::int32_t>(-7)});
    EXPECT_FALSE(r.changed);
}

/* ───────────────────────── typed accessors ───────────────────────── */
// data_store::to_string / to_bool / to_uint32 / to_int32 / to_double —
// the helpers apps consume to avoid re-rolling std::get_if + numeric
// promotion boilerplate per field. Strict by default, with documented
// integer promotions (int32 → uint32 when ≥0; uint32 → int32 when fits;
// any integer → double).

TEST(TypedAccessors, to_string_strict) {
    EXPECT_EQ("hello", *data_store::to_string(Value{std::string("hello")}));
    EXPECT_FALSE(data_store::to_string(Value{true}).has_value());
    EXPECT_FALSE(data_store::to_string(Value{static_cast<std::uint32_t>(42)}).has_value());
    EXPECT_FALSE(data_store::to_string(Value{std::monostate{}}).has_value());
}

TEST(TypedAccessors, to_bool_strict) {
    EXPECT_TRUE(*data_store::to_bool(Value{true}));
    EXPECT_FALSE(*data_store::to_bool(Value{false}));
    // Strings like "1"/"true" are NOT coerced — by design.
    EXPECT_FALSE(data_store::to_bool(Value{std::string("true")}).has_value());
    EXPECT_FALSE(data_store::to_bool(Value{static_cast<std::uint32_t>(1)}).has_value());
}

TEST(TypedAccessors, to_uint32_accepts_int32_when_non_negative) {
    EXPECT_EQ(42u, *data_store::to_uint32(Value{static_cast<std::uint32_t>(42)}));
    EXPECT_EQ(42u, *data_store::to_uint32(Value{static_cast<std::int32_t>(42)}));
    EXPECT_FALSE(data_store::to_uint32(Value{static_cast<std::int32_t>(-1)}).has_value());
    EXPECT_FALSE(data_store::to_uint32(Value{1.5}).has_value());
}

TEST(TypedAccessors, to_int32_accepts_uint32_when_fits) {
    EXPECT_EQ(-7, *data_store::to_int32(Value{static_cast<std::int32_t>(-7)}));
    EXPECT_EQ(42, *data_store::to_int32(Value{static_cast<std::uint32_t>(42)}));
    // Uint32 above INT32_MAX cannot fit → nullopt.
    EXPECT_FALSE(data_store::to_int32(
        Value{static_cast<std::uint32_t>(0x80000000u)}).has_value());
}

TEST(TypedAccessors, to_double_accepts_all_numeric) {
    EXPECT_DOUBLE_EQ(1.5, *data_store::to_double(Value{1.5}));
    EXPECT_DOUBLE_EQ(42.0, *data_store::to_double(Value{static_cast<std::uint32_t>(42)}));
    EXPECT_DOUBLE_EQ(-7.0, *data_store::to_double(Value{static_cast<std::int32_t>(-7)}));
    EXPECT_FALSE(data_store::to_double(Value{std::string("1.5")}).has_value());
    EXPECT_FALSE(data_store::to_double(Value{true}).has_value());
}
