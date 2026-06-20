#include <gtest/gtest.h>

#include "at_parser.hpp"

using namespace cellular;

TEST(AtCsq, MidScale) {
    Signal s = parse_csq("+CSQ: 24,0");
    EXPECT_TRUE(s.valid);
    EXPECT_EQ(s.dbm, -113 + 2 * 24);   // -65 dBm
    EXPECT_EQ(s.bars, 5);
}

TEST(AtCsq, WeakAndUnknown) {
    Signal weak = parse_csq("+CSQ: 2,0");
    EXPECT_TRUE(weak.valid);
    EXPECT_EQ(weak.dbm, -109);
    EXPECT_EQ(weak.bars, 0);

    EXPECT_FALSE(parse_csq("+CSQ: 99,99").valid);   // not detectable
    EXPECT_FALSE(parse_csq("garbage").valid);
}

TEST(AtCops, NameAndTech) {
    Operator op = parse_cops("+COPS: 0,0,\"Vodafone\",7");
    EXPECT_TRUE(op.valid);
    EXPECT_EQ(op.name, "Vodafone");
    EXPECT_EQ(op.tech, "4G");

    Operator g3 = parse_cops("+COPS: 0,0,\"O2 - UK\",2");
    EXPECT_EQ(g3.name, "O2 - UK");
    EXPECT_EQ(g3.tech, "3G");

    EXPECT_FALSE(parse_cops("+COPS: 0").valid);     // not registered to an operator
}

TEST(AtCreg, States) {
    EXPECT_EQ(parse_creg("+CREG: 0,1"), Reg::Home);
    EXPECT_EQ(parse_creg("+CREG: 2,5"), Reg::Roaming);
    EXPECT_EQ(parse_creg("+CEREG: 1,2"), Reg::Searching);
    EXPECT_EQ(parse_creg("+CGREG: 0,3"), Reg::Denied);
    EXPECT_EQ(parse_creg("+CEREG: 5"), Reg::Roaming);     // solicited, single field
    EXPECT_EQ(parse_creg("+CREG: 0,0"), Reg::NotRegistered);
    EXPECT_STREQ(reg_str(Reg::Home), "home");
    EXPECT_STREQ(reg_str(Reg::Roaming), "roaming");
}

TEST(AtCgpaddr, ExtractsIp) {
    EXPECT_EQ(parse_cgpaddr("+CGPADDR: 1,\"10.181.22.7\""), "10.181.22.7");
    EXPECT_EQ(parse_cgpaddr("+CGPADDR: 1,100.92.3.44"), "100.92.3.44");
    EXPECT_EQ(parse_cgpaddr("+CGPADDR: 1,\"0.0.0.0\""), "");   // no context yet
    EXPECT_EQ(parse_cgpaddr("+CGPADDR: 1"), "");
}

TEST(AtIccid, AcceptsLongDigits) {
    EXPECT_EQ(parse_iccid("+QCCID: 8944500207123456789"), "8944500207123456789");
    EXPECT_EQ(parse_iccid("+CCID: 8901234567890123456F"), "8901234567890123456F");
    EXPECT_EQ(parse_iccid("8944500207123456789"), "8944500207123456789");  // bare
    EXPECT_EQ(parse_iccid("ERROR"), "");
}
