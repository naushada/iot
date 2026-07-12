/// smsctl session engine — login, TTL, brute-force lockout, allowlist,
/// factory-reset nonce. The clock is injected, so nothing sleeps.

#include <gtest/gtest.h>

#include "smsctl/session.hpp"

using namespace smsctl;

namespace {

constexpr const char* kSender = "+919096383701";

/// admin/s3cret + viewer/looksee
AccountLookup make_lookup() {
    return [](const std::string& id, Account& out) {
        if (id == "admin") {
            out = {"admin", sha256_hex("s3cret"), "Admin"};
            return true;
        }
        if (id == "viewer") {
            out = {"viewer", sha256_hex("looksee"), "Viewer"};
            return true;
        }
        return false;
    };
}

SessionStore make_store(Config c = {}) { return SessionStore(std::move(c)); }

} // namespace

TEST(Session, Sha256MatchesHttpdHashing) {
    // The device-ui default admin password is "admin"; iot-httpd stores this
    // exact hash. SMS login must accept the same passwords, so the hashes must
    // agree byte-for-byte.
    EXPECT_EQ(sha256_hex("admin"),
              "8c6976e5b5410415bde908bd4dee15dfb167a9c873fc4bb8a81f6f2ab448a918");
}

TEST(Session, LoginSucceedsAndOpensSession) {
    auto st = make_store();
    EXPECT_EQ(st.login(kSender, "admin", "s3cret", make_lookup(), 1000), "");
    const Account* a = st.session(kSender, 1000);
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->id, "admin");
    EXPECT_EQ(a->access, "Admin");
    EXPECT_EQ(st.session_count(), 1u);
}

TEST(Session, WrongPasswordAndUnknownUserAreIndistinguishable) {
    auto st = make_store();
    const auto bad_pw   = st.login(kSender, "admin", "wrong", make_lookup(), 1000);
    const auto no_user  = st.login(kSender, "nobody", "x",    make_lookup(), 1000);
    EXPECT_EQ(bad_pw, "invalid credentials");
    EXPECT_EQ(no_user, bad_pw);          // never confirm which accounts exist
    EXPECT_EQ(st.session(kSender, 1000), nullptr);
}

TEST(Session, EmptyPasswordIsRejected) {
    auto st = make_store();
    EXPECT_EQ(st.login(kSender, "admin", "", make_lookup(), 1000),
              "invalid credentials");
}

TEST(Session, SessionExpiresAfterTtl) {
    Config c; c.session_ttl_sec = 600;
    auto st = make_store(c);
    ASSERT_EQ(st.login(kSender, "admin", "s3cret", make_lookup(), 1000), "");
    EXPECT_NE(st.session(kSender, 1000 + 599), nullptr);
    EXPECT_EQ(st.session(kSender, 1000 + 600), nullptr);   // exactly at TTL
    st.sweep(1000 + 601);
    EXPECT_EQ(st.session_count(), 0u);
}

TEST(Session, LogoutDropsTheSession) {
    auto st = make_store();
    ASSERT_EQ(st.login(kSender, "admin", "s3cret", make_lookup(), 1000), "");
    st.logout(kSender);
    EXPECT_EQ(st.session(kSender, 1000), nullptr);
}

TEST(Session, LockoutAfterNFailuresThenRecovers) {
    Config c; c.lockout_failures = 3; c.lockout_sec = 900;
    auto st = make_store(c);
    auto lk = make_lookup();

    EXPECT_EQ(st.login(kSender, "admin", "no", lk, 100), "invalid credentials");
    EXPECT_EQ(st.login(kSender, "admin", "no", lk, 101), "invalid credentials");
    // 3rd failure trips the lockout
    EXPECT_EQ(st.login(kSender, "admin", "no", lk, 102), "locked out (15 min)");
    // even the CORRECT password is refused while locked out
    EXPECT_EQ(st.login(kSender, "admin", "s3cret", lk, 200), "locked out (14 min)");
    EXPECT_EQ(st.session(kSender, 200), nullptr);

    // after the window, the right password works again
    EXPECT_EQ(st.login(kSender, "admin", "s3cret", lk, 102 + 900 + 1), "");
    EXPECT_NE(st.session(kSender, 102 + 900 + 1), nullptr);
}

TEST(Session, LockoutIsPerSender) {
    Config c; c.lockout_failures = 2;
    auto st = make_store(c);
    auto lk = make_lookup();
    st.login("+1111111111", "admin", "no", lk, 100);
    st.login("+1111111111", "admin", "no", lk, 101);      // that number is locked
    // A different number is unaffected.
    EXPECT_EQ(st.login("+2222222222", "admin", "s3cret", lk, 102), "");
}

TEST(Session, SuccessfulLoginClearsFailureCount) {
    Config c; c.lockout_failures = 3;
    auto st = make_store(c);
    auto lk = make_lookup();
    st.login(kSender, "admin", "no", lk, 100);
    st.login(kSender, "admin", "no", lk, 101);
    ASSERT_EQ(st.login(kSender, "admin", "s3cret", lk, 102), "");
    // The two earlier failures must not carry over into a new lockout.
    EXPECT_EQ(st.login(kSender, "admin", "no", lk, 103), "invalid credentials");
    EXPECT_EQ(st.login(kSender, "admin", "no", lk, 104), "invalid credentials");
}

TEST(Session, AllowlistGate) {
    Config c;
    c.allowed_numbers = {"+919096383701"};
    auto st = make_store(c);
    EXPECT_TRUE(st.sender_allowed("+919096383701"));
    EXPECT_FALSE(st.sender_allowed("+4915112345678"));
    EXPECT_FALSE(st.sender_allowed("AZ-AIRTEL-S"));      // alphanumeric sender
}

TEST(Session, AllowlistMatchesNationalAndInternationalForms) {
    // The operator stores +91909…, the carrier may deliver 909… (or vice versa).
    // Locking the operator out of their own device over a country code would be
    // a cruel failure mode.
    Config c;
    c.allowed_numbers = {"+919096383701"};
    auto st = make_store(c);
    EXPECT_TRUE(st.sender_allowed("9096383701"));
    EXPECT_TRUE(st.sender_allowed("919096383701"));
    EXPECT_FALSE(st.sender_allowed("+919000000000"));
}

TEST(Session, EmptyAllowlistPermitsAnySender) {
    auto st = make_store();                              // no allowlist
    EXPECT_TRUE(st.sender_allowed("+4915112345678"));
    EXPECT_TRUE(st.sender_allowed("anything"));
}

TEST(Session, NonceRoundTrip) {
    auto st = make_store();
    const std::string n = st.mint_nonce(kSender, 1000, 987654321);
    EXPECT_EQ(n.size(), 6u);
    for (char ch : n) EXPECT_TRUE(ch >= '0' && ch <= '9');
    EXPECT_TRUE(st.consume_nonce(kSender, n, 1000 + 10));
}

TEST(Session, NonceIsSingleUse) {
    auto st = make_store();
    const std::string n = st.mint_nonce(kSender, 1000, 42);
    ASSERT_TRUE(st.consume_nonce(kSender, n, 1001));
    EXPECT_FALSE(st.consume_nonce(kSender, n, 1002));    // replay refused
}

TEST(Session, WrongNonceBurnsIt) {
    // A wrong guess must not leave the nonce alive for another attempt.
    auto st = make_store();
    const std::string n = st.mint_nonce(kSender, 1000, 42);
    EXPECT_FALSE(st.consume_nonce(kSender, "000000", 1001));
    EXPECT_FALSE(st.consume_nonce(kSender, n, 1002));
}

TEST(Session, NonceExpires) {
    auto st = make_store();
    const std::string n = st.mint_nonce(kSender, 1000, 42);
    EXPECT_FALSE(st.consume_nonce(kSender, n,
                                  1000 + SessionStore::kNonceTtlSec + 1));
}

TEST(Session, NonceIsBoundToItsSender) {
    auto st = make_store();
    const std::string n = st.mint_nonce(kSender, 1000, 42);
    EXPECT_FALSE(st.consume_nonce("+4915112345678", n, 1001));
}
