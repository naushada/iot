/// Unit tests for the operator audit-log helpers (audit.{hpp,cpp}). Pure JSON.

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "audit.hpp"

using http_server::AuditEntry;
using http_server::append_audit;
using http_server::audit_action_for_key;
using nlohmann::json;

static AuditEntry mk(long ts, const std::string& actor, const std::string& action,
                     const std::string& target) {
    AuditEntry e; e.ts = ts; e.actor = actor; e.tenant = "acme";
    e.action = action; e.target = target; return e;
}

TEST(AuditActionForKey, MapsOnlyAuditableKeys) {
    EXPECT_EQ(audit_action_for_key("cloud.provision.request"),   "device.provision");
    EXPECT_EQ(audit_action_for_key("cloud.deprovision.request"), "device.deprovision");
    EXPECT_EQ(audit_action_for_key("cloud.tenants"),             "tenant.update");
    EXPECT_EQ(audit_action_for_key("cloud.transfer.release.request"), "device.transfer");
    EXPECT_EQ(audit_action_for_key("cloud.bs.master.key"),       "bs.master.update");
    // Non-auditable keys → "".
    EXPECT_EQ(audit_action_for_key("log.level"), "");
    EXPECT_EQ(audit_action_for_key("cloud.vpn.subnet"), "");
}

TEST(AppendAudit, NewestFirstWithFields) {
    std::string log = "[]";
    log = append_audit(log, mk(100, "alice", "device.provision", "ser1"));
    log = append_audit(log, mk(200, "bob",   "tenant.update",     "acme"));
    auto j = json::parse(log);
    ASSERT_EQ(j.size(), 2u);
    // Newest (ts=200) is at the front.
    EXPECT_EQ(j[0]["ts"], 200);
    EXPECT_EQ(j[0]["actor"], "bob");
    EXPECT_EQ(j[0]["action"], "tenant.update");
    EXPECT_EQ(j[1]["ts"], 100);
    EXPECT_EQ(j[1]["target"], "ser1");
    EXPECT_EQ(j[1]["tenant"], "acme");
}

TEST(AppendAudit, OmitsEmptyDetailKeepsNonEmpty) {
    auto plain = json::parse(append_audit("[]", mk(1, "a", "x", "t")));
    EXPECT_FALSE(plain[0].contains("detail"));
    AuditEntry e = mk(2, "a", "x", "t"); e.detail = "quota=5";
    auto withd = json::parse(append_audit("[]", e));
    EXPECT_EQ(withd[0]["detail"], "quota=5");
}

TEST(AppendAudit, RingBufferCaps) {
    std::string log = "[]";
    for (long i = 0; i < 10; ++i)
        log = append_audit(log, mk(i, "a", "device.provision", "s"), /*cap*/3);
    auto j = json::parse(log);
    ASSERT_EQ(j.size(), 3u);
    // Only the 3 newest survive (ts 9,8,7), newest-first.
    EXPECT_EQ(j[0]["ts"], 9);
    EXPECT_EQ(j[1]["ts"], 8);
    EXPECT_EQ(j[2]["ts"], 7);
}

TEST(AppendAudit, CorruptOrNonArrayInputSelfHeals) {
    // A non-array / unparseable existing value is treated as an empty log.
    auto a = json::parse(append_audit("not json", mk(1, "a", "x", "t")));
    EXPECT_EQ(a.size(), 1u);
    auto b = json::parse(append_audit("{\"oops\":1}", mk(1, "a", "x", "t")));
    EXPECT_EQ(b.size(), 1u);
}
