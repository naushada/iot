/// D1 risk-gate tests for the data-store module.

#include <gtest/gtest.h>

#include "data_store/proto.hpp"
#include "../src/server/data_store.hpp"

using data_store::server::DataStore;
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
    auto prev = s.set("k", "v");
    EXPECT_FALSE(prev.has_value());
    EXPECT_EQ(1u, s.size());
    auto v = s.get("k");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ("v", *v);
}

TEST(DataStore, second_set_returns_previous) {
    DataStore s;
    s.set("k", "v1");
    auto prev = s.set("k", "v2");
    ASSERT_TRUE(prev.has_value());
    EXPECT_EQ("v1", *prev);
    EXPECT_EQ("v2", *s.get("k"));
    EXPECT_EQ(1u, s.size());
}

TEST(DataStore, remove_returns_true_only_when_present) {
    DataStore s;
    s.set("k", "v");
    EXPECT_TRUE(s.remove("k"));
    EXPECT_FALSE(s.remove("k"));
    EXPECT_FALSE(s.get("k").has_value());
}
