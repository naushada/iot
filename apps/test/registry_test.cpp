#include <gtest/gtest.h>

#include "lwm2m_registration.hpp"

using ::lwm2m::ClientRegistry;
using ::lwm2m::ServerRegistration;
using ::lwm2m::linkformat::LinkEntry;

namespace {

ServerRegistration make_reg(const std::string& ep,
                            std::uint32_t lt = 86400,
                            std::uint16_t ssid = 1) {
    ServerRegistration r;
    r.endpoint        = ep;
    r.shortServerId   = ssid;
    r.lifetime        = lt;
    r.binding         = "U";
    r.lwm2mVersion    = "1.1";
    r.peerHost        = "10.0.0.5";
    r.peerPort        = 56830;
    return r;
}

} // namespace

/* ─────────────────────────── REQ-REG-009 + REQ-REG-011 ───────────────── */

TEST(Registry, REQ_REG_009_add_assigns_location_and_expiry) {
    ClientRegistry reg;
    auto t0 = std::chrono::steady_clock::now();
    auto loc = reg.add(make_reg("urn:dev:1", /*lt*/ 100), t0);

    ASSERT_FALSE(loc.empty());
    EXPECT_EQ("/rd/1", loc);

    auto* r = reg.find(loc);
    ASSERT_NE(nullptr, r);
    EXPECT_EQ("urn:dev:1", r->endpoint);
    EXPECT_EQ(loc, r->location);
    EXPECT_EQ(t0, r->registeredAt);
    EXPECT_EQ(t0 + std::chrono::seconds(100), r->expiresAt);
    // D2 keying: ssid carried through.
    EXPECT_EQ(1u, r->shortServerId);
}

TEST(Registry, REQ_REG_011_short_server_id_preserved) {
    ClientRegistry reg;
    auto loc = reg.add(make_reg("urn:dev:42", 86400, /*ssid*/ 7));
    ASSERT_NE(nullptr, reg.find(loc));
    EXPECT_EQ(7u, reg.find(loc)->shortServerId);
}

TEST(Registry, REQ_REG_009_locations_are_unique_within_session) {
    ClientRegistry reg;
    auto a = reg.add(make_reg("urn:a"));
    auto b = reg.add(make_reg("urn:b"));
    auto c = reg.add(make_reg("urn:c"));
    EXPECT_NE(a, b);
    EXPECT_NE(b, c);
    EXPECT_EQ(3u, reg.size());
}

TEST(Registry, REQ_REG_003_update_refreshes_lifetime) {
    ClientRegistry reg;
    auto t0 = std::chrono::steady_clock::now();
    auto loc = reg.add(make_reg("urn:dev:1", /*lt*/ 100), t0);

    auto t1 = t0 + std::chrono::seconds(50);
    ASSERT_TRUE(reg.update(loc, /*lt*/ 200, /*binding*/ "", nullptr, t1));

    auto* r = reg.find(loc);
    ASSERT_NE(nullptr, r);
    EXPECT_EQ(200u, r->lifetime);
    EXPECT_EQ(t1 + std::chrono::seconds(200), r->expiresAt);
}

TEST(Registry, REQ_REG_003_update_can_keep_lifetime_with_lt_zero) {
    ClientRegistry reg;
    auto t0 = std::chrono::steady_clock::now();
    auto loc = reg.add(make_reg("urn:dev:1", 100), t0);

    auto t1 = t0 + std::chrono::seconds(60);
    ASSERT_TRUE(reg.update(loc, /*lt*/ 0, "", nullptr, t1));
    auto* r = reg.find(loc);
    ASSERT_NE(nullptr, r);
    EXPECT_EQ(100u, r->lifetime);                                // unchanged
    EXPECT_EQ(t1 + std::chrono::seconds(100), r->expiresAt);     // re-pinned
}

TEST(Registry, REQ_REG_003_update_advertised_set) {
    ClientRegistry reg;
    auto loc = reg.add(make_reg("urn:dev:1"));

    std::vector<LinkEntry> newSet;
    LinkEntry e;  e.uri = "/3/0";
    newSet.push_back(e);
    ASSERT_TRUE(reg.update(loc, 0, "", &newSet));
    ASSERT_EQ(1u, reg.find(loc)->advertisedSet.size());
    EXPECT_EQ("/3/0", reg.find(loc)->advertisedSet[0].uri);
}

TEST(Registry, REQ_REG_004_remove_returns_true_on_known_location) {
    ClientRegistry reg;
    auto loc = reg.add(make_reg("urn:dev:1"));
    EXPECT_TRUE(reg.remove(loc));
    EXPECT_EQ(nullptr, reg.find(loc));
    EXPECT_FALSE(reg.remove(loc));    // already gone
}

/* ─────────────────────────── REQ-REG-005 expiry ──────────────────────── */

TEST(Registry, REQ_REG_005_expire_drops_past_due) {
    ClientRegistry reg;
    auto t0 = std::chrono::steady_clock::now();
    auto a = reg.add(make_reg("urn:a", 10), t0);
    auto b = reg.add(make_reg("urn:b", 60), t0);

    auto now = t0 + std::chrono::seconds(11);
    auto expired = reg.expire(now);

    ASSERT_EQ(1u, expired.size());
    EXPECT_EQ(a, expired[0]);
    EXPECT_EQ(nullptr, reg.find(a));
    EXPECT_NE(nullptr, reg.find(b));
}

TEST(Registry, REQ_REG_005_expire_drift_under_500ms_after_one_lifetime) {
    // L3 risk gate: lifetime timer must not drift > 500 ms on a 1500 s
    // lifetime. The drift is bounded by the 1 Hz reactor tick + the
    // monotonic clock; this test pins the contract at the registry level.
    ClientRegistry reg;
    auto t0 = std::chrono::steady_clock::now();
    auto loc = reg.add(make_reg("urn:dev:long", 1500), t0);

    // 500 ms before expiry → not yet expired.
    auto justBefore = t0 + std::chrono::milliseconds(1499500);
    EXPECT_TRUE(reg.expire(justBefore).empty());

    // 500 ms after expiry → expired.
    auto justAfter = t0 + std::chrono::milliseconds(1500500);
    auto expired = reg.expire(justAfter);
    ASSERT_EQ(1u, expired.size());
    EXPECT_EQ(loc, expired[0]);
}

/* ─────────────────────────── D3 reconstruction ───────────────────────── */

TEST(Registry, D3_load_from_preserves_locations) {
    std::vector<ServerRegistration> snap;
    auto a = make_reg("urn:a", 100);  a.location = "/rd/abc";
    auto b = make_reg("urn:b", 200);  b.location = "/rd/def";
    snap.push_back(a);
    snap.push_back(b);

    ClientRegistry reg;
    reg.load_from(std::move(snap));
    EXPECT_EQ(2u, reg.size());
    EXPECT_NE(nullptr, reg.find("/rd/abc"));
    EXPECT_NE(nullptr, reg.find("/rd/def"));
    EXPECT_EQ("urn:a", reg.find("/rd/abc")->endpoint);
}
