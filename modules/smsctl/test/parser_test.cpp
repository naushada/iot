/// smsctl parser — the `IOT …` SMS command grammar.

#include <gtest/gtest.h>

#include "smsctl/parser.hpp"

using namespace smsctl;

TEST(Parser, NonIotTextIsNotACommand) {
    // Ordinary human text and carrier spam MUST be silently ignored — a reply
    // would burn credit and could start a loop.
    EXPECT_EQ(parse("hello there").kind, Kind::NotACommand);
    EXPECT_EQ(parse("Welcome to Airtel 5G Plus. Recharge now!").kind, Kind::NotACommand);
    EXPECT_EQ(parse("").kind, Kind::NotACommand);
    EXPECT_EQ(parse("   ").kind, Kind::NotACommand);
    EXPECT_EQ(parse("IOTA STATUS").kind, Kind::NotACommand);   // prefix must be its own token
}

TEST(Parser, KeywordsAreCaseInsensitive) {
    EXPECT_EQ(parse("IOT STATUS").kind, Kind::Status);
    EXPECT_EQ(parse("iot status").kind, Kind::Status);
    EXPECT_EQ(parse("Iot Status").kind, Kind::Status);
    EXPECT_EQ(parse("  iot   status  ").kind, Kind::Status);   // sloppy spacing
}

TEST(Parser, Login) {
    auto c = parse("IOT LOGIN admin s3cret");
    ASSERT_EQ(c.kind, Kind::Login);
    ASSERT_EQ(c.args.size(), 2u);
    EXPECT_EQ(c.args[0], "admin");
    EXPECT_EQ(c.args[1], "s3cret");
}

TEST(Parser, LoginPasswordKeepsItsCase) {
    // Arguments must NOT be upper-cased — passwords/SSIDs are case-sensitive.
    auto c = parse("iot login Alice PaSsWoRd");
    ASSERT_EQ(c.kind, Kind::Login);
    EXPECT_EQ(c.args[0], "Alice");
    EXPECT_EQ(c.args[1], "PaSsWoRd");
}

TEST(Parser, LoginWrongArityIsUnknown) {
    EXPECT_EQ(parse("IOT LOGIN admin").kind, Kind::Unknown);
    EXPECT_EQ(parse("IOT LOGIN").kind, Kind::Unknown);
    EXPECT_EQ(parse("IOT LOGIN a b c").kind, Kind::Unknown);
}

TEST(Parser, ParseErrorNeverEchoesArguments) {
    // The error text ends up in a reply SMS — it must not leak a password.
    auto c = parse("IOT LOGIN admin hunter2 extra");
    ASSERT_EQ(c.kind, Kind::Unknown);
    EXPECT_EQ(c.error.find("hunter2"), std::string::npos);
    // An unknown verb must not be echoed back either (no reflected content).
    auto u = parse("IOT DELETE-EVERYTHING now");
    ASSERT_EQ(u.kind, Kind::Unknown);
    EXPECT_EQ(u.error.find("DELETE"), std::string::npos);
}

TEST(Parser, SimpleCommands) {
    EXPECT_EQ(parse("IOT LOGOUT").kind, Kind::Logout);
    EXPECT_EQ(parse("IOT REBOOT").kind, Kind::Reboot);
    EXPECT_EQ(parse("IOT RADIO RESTART").kind, Kind::RadioRestart);
    EXPECT_EQ(parse("iot radio restart").kind, Kind::RadioRestart);
    EXPECT_EQ(parse("IOT RADIO").kind, Kind::Unknown);          // needs RESTART
    EXPECT_EQ(parse("IOT RADIO STOP").kind, Kind::Unknown);
}

TEST(Parser, Apn) {
    auto c = parse("IOT APN airtelgprs.com");
    ASSERT_EQ(c.kind, Kind::Apn);
    ASSERT_EQ(c.args.size(), 1u);
    EXPECT_EQ(c.args[0], "airtelgprs.com");
    EXPECT_EQ(parse("IOT APN").kind, Kind::Unknown);
}

TEST(Parser, FactoryResetIsOneOrTwoStep) {
    auto step1 = parse("IOT FACTORY-RESET");
    ASSERT_EQ(step1.kind, Kind::FactoryReset);
    EXPECT_TRUE(step1.args.empty());

    auto step2 = parse("IOT FACTORY-RESET 123456");
    ASSERT_EQ(step2.kind, Kind::FactoryReset);
    ASSERT_EQ(step2.args.size(), 1u);
    EXPECT_EQ(step2.args[0], "123456");

    EXPECT_EQ(parse("IOT FACTORYRESET").kind, Kind::FactoryReset);  // no-hyphen spelling
}

TEST(Parser, WifiWithAndWithoutPsk) {
    auto c = parse("IOT WIFI HomeNet secretpass");
    ASSERT_EQ(c.kind, Kind::Wifi);
    ASSERT_EQ(c.args.size(), 2u);
    EXPECT_EQ(c.args[0], "HomeNet");
    EXPECT_EQ(c.args[1], "secretpass");

    auto open = parse("IOT WIFI GuestNet");      // no psk → open network
    ASSERT_EQ(open.kind, Kind::Wifi);
    ASSERT_EQ(open.args.size(), 1u);

    EXPECT_EQ(parse("IOT WIFI").kind, Kind::Unknown);
    EXPECT_EQ(parse("IOT WIFI a b c").kind, Kind::Unknown);
}

TEST(Parser, QuotedArgumentsCarrySpaces) {
    // The whole reason for quoting: real SSIDs and PSKs contain spaces.
    auto c = parse("IOT WIFI \"My Home AP\" \"pass with spaces\"");
    ASSERT_EQ(c.kind, Kind::Wifi);
    ASSERT_EQ(c.args.size(), 2u);
    EXPECT_EQ(c.args[0], "My Home AP");
    EXPECT_EQ(c.args[1], "pass with spaces");
}

TEST(Parser, EscapedQuoteInsideArgument) {
    auto c = parse("IOT WIFI \"AP\\\"X\" pw");
    ASSERT_EQ(c.kind, Kind::Wifi);
    EXPECT_EQ(c.args[0], "AP\"X");
}

TEST(Parser, UnterminatedQuoteIsForgiven) {
    // A phone keyboard may swallow the closing quote — take the rest of the line
    // rather than rejecting the operator's only lifeline to the device.
    auto c = parse("IOT WIFI \"My AP");
    ASSERT_EQ(c.kind, Kind::Wifi);
    EXPECT_EQ(c.args[0], "My AP");
}

TEST(Parser, TokenizerBasics) {
    auto t = tokenize("a  b\tc");
    ASSERT_EQ(t.size(), 3u);
    EXPECT_EQ(t[0], "a");
    EXPECT_EQ(t[2], "c");
    EXPECT_TRUE(tokenize("").empty());
}

TEST(Parser, MutatingClassification) {
    EXPECT_TRUE(is_mutating(Kind::Reboot));
    EXPECT_TRUE(is_mutating(Kind::FactoryReset));
    EXPECT_TRUE(is_mutating(Kind::Apn));
    EXPECT_TRUE(is_mutating(Kind::RadioRestart));
    EXPECT_TRUE(is_mutating(Kind::Wifi));
    EXPECT_FALSE(is_mutating(Kind::Status));
    EXPECT_FALSE(is_mutating(Kind::Login));
    EXPECT_FALSE(is_mutating(Kind::Logout));
}
