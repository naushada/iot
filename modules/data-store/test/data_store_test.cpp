/// D1 risk-gate tests for the data-store module.

#include <gtest/gtest.h>

#include "data_store/proto.hpp"
#include "data_store/value.hpp"
#include "../src/server/data_store.hpp"

using data_store::server::DataStore;
using data_store::Value;
namespace proto = data_store::proto;

/* ───────────────────────────── proto ──────────────────────────────── */

TEST(Proto, parse_op_round_trip) {
    EXPECT_EQ(proto::Op::Set,      proto::parse_op("set"));
    EXPECT_EQ(proto::Op::Get,      proto::parse_op("get"));
    EXPECT_EQ(proto::Op::Register, proto::parse_op("register"));
    EXPECT_EQ(proto::Op::Remove,   proto::parse_op("remove"));
    EXPECT_EQ(proto::Op::Unknown,  proto::parse_op("fetch"));
    EXPECT_EQ(proto::Op::Unknown,  proto::parse_op(""));
}

TEST(Proto, op_name_matches_parser) {
    for (auto op : {proto::Op::Set, proto::Op::Get,
                    proto::Op::Register, proto::Op::Remove}) {
        EXPECT_EQ(op, proto::parse_op(proto::op_name(op)));
    }
}

TEST(Proto, welcome_line_is_one_json_doc_ending_in_newline) {
    std::string w{proto::kWelcomeLine};
    ASSERT_FALSE(w.empty());
    EXPECT_EQ('\n', w.back());
    EXPECT_NE(std::string::npos, w.find("\"ok\":true"));
    EXPECT_NE(std::string::npos, w.find("data-store-server"));
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
