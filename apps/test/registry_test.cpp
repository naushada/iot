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

TEST(Registry, reregister_same_endpoint_reuses_location_no_duplicate) {
    ClientRegistry reg;
    auto first = reg.add(make_reg("urn:dev:1", /*lt*/ 100));
    EXPECT_EQ(1u, reg.size());

    // Same endpoint re-registers (device went offline and came back) from a
    // NEW public/ISP address before the old registration expired.
    auto again = make_reg("urn:dev:1", /*lt*/ 100);
    again.peerHost = "203.0.113.9";
    again.peerPort = 41000;
    auto second = reg.add(again);

    EXPECT_EQ(first, second);                 // location reused, not churned
    EXPECT_EQ(1u, reg.size());                // replaced in place, no duplicate
    auto* rec = reg.find(first);
    ASSERT_NE(nullptr, rec);
    EXPECT_EQ("203.0.113.9", rec->peerHost);  // record reflects the new peer
    EXPECT_EQ(41000u, rec->peerPort);
}

TEST(Registry, update_refreshes_peer_address_but_empty_does_not_clobber) {
    ClientRegistry reg;
    auto loc = reg.add(make_reg("urn:dev:1", /*lt*/ 100));

    // Keepalive Update from a new NAT address → recorded peer (ISP IP) follows.
    ASSERT_TRUE(reg.update(loc, 0, "", nullptr, "198.51.100.7", 33333));
    auto* r = reg.find(loc);
    ASSERT_NE(nullptr, r);
    EXPECT_EQ("198.51.100.7", r->peerHost);
    EXPECT_EQ(33333u, r->peerPort);

    // Plain-UDP dispatch path passes an empty peer — must NOT wipe the address.
    ASSERT_TRUE(reg.update(loc, 0, "", nullptr, "", 0));
    EXPECT_EQ("198.51.100.7", reg.find(loc)->peerHost);
}

TEST(Registry, REQ_REG_003_update_refreshes_lifetime) {
    ClientRegistry reg;
    auto t0 = std::chrono::steady_clock::now();
    auto loc = reg.add(make_reg("urn:dev:1", /*lt*/ 100), t0);

    auto t1 = t0 + std::chrono::seconds(50);
    ASSERT_TRUE(reg.update(loc, /*lt*/ 200, /*binding*/ "", nullptr,
                           /*peerHost*/ "", /*peerPort*/ 0, t1));

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
    ASSERT_TRUE(reg.update(loc, /*lt*/ 0, "", nullptr,
                           /*peerHost*/ "", /*peerPort*/ 0, t1));
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

/* ── Stable /rd/{location} per endpoint across reboot/expiry/restart ───────── */

TEST(Registry, reregister_after_expiry_reuses_same_location) {
    ClientRegistry reg;
    auto t0 = std::chrono::steady_clock::now();
    auto first = reg.add(make_reg("urn:dev:1", /*lt*/ 100), t0);

    // Device offline long enough that the registration expires and is reaped.
    auto reaped = reg.expire(t0 + std::chrono::seconds(101));
    ASSERT_EQ(1u, reaped.size());
    EXPECT_EQ(0u, reg.size());

    // It reboots and re-registers — same location, not a churned new one.
    auto again = reg.add(make_reg("urn:dev:1", 100), t0 + std::chrono::seconds(300));
    EXPECT_EQ(first, again);
    EXPECT_EQ(1u, reg.size());
}

TEST(Registry, reregister_after_deregister_reuses_same_location) {
    ClientRegistry reg;
    auto first = reg.add(make_reg("urn:dev:1"));
    ASSERT_TRUE(reg.remove(first));
    EXPECT_EQ(0u, reg.size());

    auto again = reg.add(make_reg("urn:dev:1"));
    EXPECT_EQ(first, again);                  // deregister doesn't forget the loc
    EXPECT_EQ(1u, reg.size());
}

TEST(Registry, load_from_rehydrates_endpoint_location_and_advances_counter) {
    // Server restart: rehydrate from Mongo, then the device re-registers.
    ClientRegistry reg;
    ServerRegistration persisted = make_reg("urn:dev:7");
    persisted.location = "/rd/42";            // counter-based id minted before restart
    reg.load_from({persisted});

    // Re-register of the rehydrated endpoint reuses its persisted location.
    EXPECT_EQ("/rd/42", reg.add(make_reg("urn:dev:7")));

    // A brand-new endpoint mints ABOVE the loaded max — never reissues /rd/42.
    auto fresh = reg.add(make_reg("urn:dev:new"));
    EXPECT_NE("/rd/42", fresh);
    EXPECT_EQ(2u, reg.size());
}
