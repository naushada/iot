/// smsctl executor — auth gating and the exact ds writes / trigger files each
/// command produces. Runs against a mock sink: no ds server, no modem.

#include <gtest/gtest.h>

#include <map>
#include <set>

#include <nlohmann/json.hpp>

#include "smsctl/executor.hpp"
#include "smsctl/parser.hpp"

using namespace smsctl;
using json = nlohmann::json;

namespace {

constexpr const char* kSender = "+919096383701";

class MockSink : public DsSink {
public:
    std::map<std::string, std::string> store;
    std::map<std::string, std::string> triggers;   // path → content
    std::set<std::string>              write_fail; // keys whose set() fails
    std::set<std::string>              trigger_fail;
    std::uint64_t                      clock_ms = 1'700'000'000'000ULL;

    bool set(const std::string& key, const std::string& value) override {
        if (write_fail.count(key)) return false;
        store[key] = value;
        return true;
    }
    std::optional<std::string> get(const std::string& key) override {
        auto it = store.find(key);
        if (it == store.end()) return std::nullopt;
        return it->second;
    }
    bool arm_trigger(const std::string& path, const std::string& content) override {
        if (trigger_fail.count(path)) return false;
        triggers[path] = content;
        return true;
    }
    std::uint64_t now_ms() override { return clock_ms; }
};

AccountLookup make_lookup() {
    return [](const std::string& id, Account& out) {
        if (id == "admin")  { out = {"admin",  sha256_hex("s3cret"),  "Admin"};  return true; }
        if (id == "viewer") { out = {"viewer", sha256_hex("looksee"), "Viewer"}; return true; }
        return false;
    };
}

/// Test harness: a sink + session store + executor, with a helper that runs a
/// raw SMS body end-to-end (parse → auth → execute) like the daemon does.
struct Fixture {
    MockSink     sink;
    SessionStore sessions;
    Executor     ex{sink, sessions, make_lookup()};
    std::uint64_t now = 1000;

    std::string send(const std::string& text, const std::string& from = kSender) {
        return ex.handle(parse(text), from, now, /*seed=*/424242);
    }
    /// Log in and assert it worked — every executor test starts from a session.
    void login_admin()  { require_ok(send("IOT LOGIN admin s3cret")); }
    void login_viewer() { require_ok(send("IOT LOGIN viewer looksee")); }

private:
    static void require_ok(const std::string& reply) {
        if (reply.rfind("OK", 0) != 0)
            throw std::runtime_error("login failed: " + reply);
    }
};

} // namespace

// ── auth gating ─────────────────────────────────────────────────────────────

TEST(Executor, CommandsRequireALogin) {
    Fixture f;
    EXPECT_EQ(f.send("IOT STATUS").rfind("ERR STATUS: login required", 0), 0u);
    EXPECT_EQ(f.send("IOT REBOOT").rfind("ERR REBOOT: login required", 0), 0u);
    // Nothing was executed.
    EXPECT_TRUE(f.sink.triggers.empty());
    EXPECT_TRUE(f.sink.store.empty());
}

TEST(Executor, LoginReplyNamesTheUserAndTtl) {
    Fixture f;
    EXPECT_EQ(f.send("IOT LOGIN admin s3cret"), "OK LOGIN: admin, 10 min");
}

TEST(Executor, LoginReplyNeverContainsThePassword) {
    Fixture f;
    const std::string ok  = f.send("IOT LOGIN admin s3cret");
    const std::string bad = f.send("IOT LOGIN admin wrongpw");
    EXPECT_EQ(ok.find("s3cret"), std::string::npos);
    EXPECT_EQ(bad.find("wrongpw"), std::string::npos);
    EXPECT_EQ(bad, "ERR LOGIN: invalid credentials");
}

TEST(Executor, ViewerMayReadStatusButNotMutate) {
    Fixture f;
    f.login_viewer();
    EXPECT_EQ(f.send("IOT STATUS").rfind("OK STATUS", 0), 0u);
    EXPECT_EQ(f.send("IOT REBOOT"), "ERR REBOOT: admin access required");
    EXPECT_EQ(f.send("IOT WIFI Home pw"), "ERR WIFI: admin access required");
    EXPECT_EQ(f.send("IOT APN x"), "ERR APN: admin access required");
    EXPECT_TRUE(f.sink.triggers.empty());
}

TEST(Executor, LogoutEndsTheSession) {
    Fixture f;
    f.login_admin();
    EXPECT_EQ(f.send("IOT LOGOUT"), "OK LOGOUT");
    EXPECT_EQ(f.send("IOT STATUS").rfind("ERR STATUS: login required", 0), 0u);
}

TEST(Executor, SessionIsBoundToTheSenderNumber) {
    Fixture f;
    f.login_admin();                                   // as kSender
    // A different number does not inherit that session.
    EXPECT_EQ(f.send("IOT REBOOT", "+4915112345678").rfind("ERR REBOOT: login required", 0), 0u);
}

TEST(Executor, UnknownCommandRepliesWithUsage) {
    Fixture f;
    EXPECT_EQ(f.send("IOT LOGIN admin"), "ERR: usage: IOT LOGIN <user> <password>");
}

// ── executors ───────────────────────────────────────────────────────────────

TEST(Executor, Reboot) {
    Fixture f;
    f.login_admin();
    EXPECT_EQ(f.send("IOT REBOOT"), "OK REBOOT: rebooting now");
    ASSERT_EQ(f.sink.triggers.count(kRebootTrigger), 1u);
    EXPECT_EQ(f.sink.triggers[kRebootTrigger], "reboot\n");
}

TEST(Executor, RebootReportsAFailedTrigger) {
    Fixture f;
    f.login_admin();
    f.sink.trigger_fail.insert(kRebootTrigger);
    EXPECT_EQ(f.send("IOT REBOOT"), "ERR REBOOT: cannot arm trigger");
}

TEST(Executor, RadioRestartBumpsTheResetToken) {
    Fixture f;
    f.login_admin();
    EXPECT_EQ(f.send("IOT RADIO RESTART"), "OK RADIO: restarting (CFUN cycle)");
    EXPECT_EQ(f.sink.store["cell.reset.request"], std::to_string(f.sink.clock_ms));
}

TEST(Executor, ApnSetsTheKeyAndCyclesTheRadio) {
    Fixture f;
    f.login_admin();
    EXPECT_EQ(f.send("IOT APN airtelgprs.com"),
              "OK APN: airtelgprs.com saved, restarting radio");
    EXPECT_EQ(f.sink.store["cell.apn"], "airtelgprs.com");
    // The radio cycle is what actually applies the new APN (cellular-client
    // re-reads config in start_reset).
    EXPECT_EQ(f.sink.store.count("cell.reset.request"), 1u);
}

TEST(Executor, ApnReportsAFailedDsWrite) {
    Fixture f;
    f.login_admin();
    f.sink.write_fail.insert("cell.apn");
    EXPECT_EQ(f.send("IOT APN broken.apn"), "ERR APN: ds write failed");
}

TEST(Executor, StatusRendersMissingKeysAsDash) {
    Fixture f;
    f.login_admin();
    const std::string r = f.send("IOT STATUS");
    EXPECT_EQ(r.rfind("OK STATUS: reg=- cs=- sig=-dBm ip=-", 0), 0u);
    EXPECT_LE(r.size(), Executor::kMaxReply);
}

TEST(Executor, StatusReportsLiveValues) {
    Fixture f;
    f.sink.store["cell.reg"]        = "home";
    f.sink.store["cell.reg.cs"]     = "home";
    f.sink.store["cell.signal.dbm"] = "-89";
    f.sink.store["cell.ip"]         = "100.127.175.251";
    f.sink.store["vpn.state"]       = "up";
    f.sink.store["wifi.assoc.ssid"] = "HomeNet";
    f.sink.store["net.iface.active"]= "wlan0";
    f.login_admin();
    const std::string r = f.send("IOT STATUS");
    EXPECT_NE(r.find("reg=home"), std::string::npos);
    EXPECT_NE(r.find("cs=home"), std::string::npos);      // the MT-SMS domain
    EXPECT_NE(r.find("sig=-89dBm"), std::string::npos);
    EXPECT_NE(r.find("vpn=up"), std::string::npos);
    EXPECT_LE(r.size(), Executor::kMaxReply);
}

// ── factory reset ───────────────────────────────────────────────────────────

TEST(Executor, FactoryResetNeedsTheNonce) {
    Fixture f;
    f.login_admin();
    const std::string step1 = f.send("IOT FACTORY-RESET");
    EXPECT_EQ(step1.rfind("OK FACTORY-RESET: confirm", 0), 0u);
    // Step 1 must NOT have armed anything.
    EXPECT_TRUE(f.sink.triggers.empty());

    // Extract the 6-digit code out of the reply, as the operator would.
    const auto pos = step1.find_last_of(' ');
    ASSERT_NE(pos, std::string::npos);
    const std::string nonce = step1.substr(pos + 1);
    ASSERT_EQ(nonce.size(), 6u);

    EXPECT_EQ(f.send("IOT FACTORY-RESET " + nonce),
              "OK FACTORY-RESET: wiping + rebooting");
    EXPECT_EQ(f.sink.triggers.count(kFactoryResetTrigger), 1u);
}

TEST(Executor, FactoryResetRejectsABadNonce) {
    Fixture f;
    f.login_admin();
    f.send("IOT FACTORY-RESET");                       // mints a nonce
    EXPECT_EQ(f.send("IOT FACTORY-RESET 000000"),
              "ERR FACTORY-RESET: bad or expired code");
    EXPECT_TRUE(f.sink.triggers.empty());
}

TEST(Executor, FactoryResetRejectsAnExpiredNonce) {
    Fixture f;
    f.login_admin();
    const std::string step1 = f.send("IOT FACTORY-RESET");
    const std::string nonce = step1.substr(step1.find_last_of(' ') + 1);

    // Nonce TTL (300s) is shorter than the session TTL (600s), so at +301s the
    // session is still alive and ONLY the nonce has died.
    f.now += SessionStore::kNonceTtlSec + 1;
    EXPECT_EQ(f.send("IOT FACTORY-RESET " + nonce),
              "ERR FACTORY-RESET: bad or expired code");
    EXPECT_TRUE(f.sink.triggers.empty());
}

// ── wifi ────────────────────────────────────────────────────────────────────

TEST(Executor, WifiWritesTheNetworkAndNothingElse) {
    Fixture f;
    f.login_admin();
    EXPECT_EQ(f.send("IOT WIFI \"My AP\" \"pass word\""),
              "OK WIFI: \"My AP\" saved, reconnecting");

    const auto arr = json::parse(f.sink.store["wifi.networks"]);
    ASSERT_EQ(arr.size(), 1u);
    EXPECT_EQ(arr[0]["ssid"], "My AP");
    EXPECT_EQ(arr[0]["psk"], "pass word");
    EXPECT_EQ(arr[0]["key_mgmt"], "WPA-PSK");
    // The ds write IS the command — iot-wifi-client watches wifi.networks and
    // re-applies. smsctld must NOT reach for a privileged side channel here.
    EXPECT_TRUE(f.sink.triggers.empty());
}

TEST(Executor, WifiReplyNeverContainsThePsk) {
    Fixture f;
    f.login_admin();
    const std::string r = f.send("IOT WIFI HomeNet sup3rs3cret");
    EXPECT_EQ(r.find("sup3rs3cret"), std::string::npos);
}

TEST(Executor, WifiWithoutPskIsAnOpenNetwork) {
    Fixture f;
    f.login_admin();
    f.send("IOT WIFI GuestNet");
    const auto arr = json::parse(f.sink.store["wifi.networks"]);
    EXPECT_EQ(arr[0]["key_mgmt"], "NONE");
    EXPECT_FALSE(arr[0].contains("psk"));
}

TEST(Executor, WifiReportsAFailedDsWrite) {
    Fixture f;
    f.login_admin();
    f.sink.write_fail.insert("wifi.networks");
    EXPECT_EQ(f.send("IOT WIFI Home pw"), "ERR WIFI: ds write failed");
}

// ── wifi.networks upsert (pure) ─────────────────────────────────────────────

TEST(WifiUpsert, AppendsToAnExistingArray) {
    const std::string in = R"([{"ssid":"Old","psk":"x","key_mgmt":"WPA-PSK"}])";
    const auto arr = json::parse(wifi_networks_upsert(in, "New", "y"));
    ASSERT_EQ(arr.size(), 2u);
    EXPECT_EQ(arr[0]["ssid"], "Old");
    EXPECT_EQ(arr[1]["ssid"], "New");
}

TEST(WifiUpsert, ReplacesTheSameSsidInPlace) {
    const std::string in =
        R"([{"ssid":"Home","psk":"old","key_mgmt":"WPA-PSK","priority":5},
            {"ssid":"Other","psk":"z","key_mgmt":"WPA-PSK"}])";
    const auto arr = json::parse(wifi_networks_upsert(in, "Home", "new"));
    ASSERT_EQ(arr.size(), 2u);                 // replaced, not duplicated
    EXPECT_EQ(arr[0]["ssid"], "Home");
    EXPECT_EQ(arr[0]["psk"], "new");
    EXPECT_EQ(arr[0]["priority"], 5);          // operator's priority survives
    EXPECT_EQ(arr[1]["ssid"], "Other");        // the other network is untouched
}

TEST(WifiUpsert, ToleratesGarbageInput) {
    // The operator is texting us BECAUSE WiFi is broken — refusing to write over
    // a malformed wifi.networks would strand them.
    for (const char* junk : {"", "not json", "{\"not\":\"an array\"}"}) {
        const auto arr = json::parse(wifi_networks_upsert(junk, "Home", "pw"));
        ASSERT_TRUE(arr.is_array());
        ASSERT_EQ(arr.size(), 1u);
        EXPECT_EQ(arr[0]["ssid"], "Home");
    }
}

// ── reply clamping ──────────────────────────────────────────────────────────

TEST(ClampReply, TruncatesToOneSms) {
    const std::string long_reply(300, 'x');
    const std::string out = clamp_reply(long_reply);
    EXPECT_EQ(out.size(), Executor::kMaxReply);
    EXPECT_EQ(out.substr(out.size() - 3), "...");
    EXPECT_EQ(clamp_reply("short"), "short");
}
