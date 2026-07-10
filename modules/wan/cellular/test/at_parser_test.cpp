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

TEST(AtReg, BestAcrossDomains) {
    // 2G camp: registered via +CREG, but +CEREG (LTE) reads not-registered —
    // the combined result must be the registered one, not "offline".
    EXPECT_EQ(best_reg(Reg::Roaming, Reg::Unknown, Reg::NotRegistered), Reg::Roaming);
    EXPECT_EQ(best_reg(Reg::NotRegistered, Reg::NotRegistered, Reg::Home), Reg::Home);
    EXPECT_EQ(best_reg(Reg::Searching, Reg::Denied, Reg::Unknown), Reg::Searching);
    EXPECT_EQ(best_reg(Reg::Home, Reg::Roaming, Reg::Roaming), Reg::Home);   // Home preferred
    EXPECT_EQ(best_reg(Reg::Unknown, Reg::Unknown, Reg::Unknown), Reg::Unknown);
}

TEST(AtSelrat, TokenToIndex) {
    EXPECT_EQ(selrat_index("auto"), 0);
    EXPECT_EQ(selrat_index("GSM"), 2);          // case-insensitive
    EXPECT_EQ(selrat_index("2g"), 2);
    EXPECT_EQ(selrat_index("lte"), 6);
    EXPECT_EQ(selrat_index("gsm+lte"), 12);
    EXPECT_EQ(selrat_index("nonsense"), -1);
}

TEST(AtSelrat, ParsesName) {
    EXPECT_EQ(parse_selrat("!SELRAT: 06, LTE Only"), "LTE Only");
    EXPECT_EQ(parse_selrat("!SELRAT: 00, Automatic"), "Automatic");
    EXPECT_EQ(parse_selrat("+CSQ: 12,99"), "");    // not a SELRAT line
}

TEST(AtCeer, ExtractsCause) {
    EXPECT_EQ(parse_ceer("+CEER: PLMN not allowed"), "PLMN not allowed");
    EXPECT_EQ(parse_ceer("ERROR"), "");            // firmware without CEER
}

TEST(AtIdentity, CnumImeiModelCapability) {
    EXPECT_EQ(parse_cnum("+CNUM: \"\",\"+337000023024234\",145"), "+337000023024234");
    EXPECT_EQ(parse_cnum("OK"), "");

    EXPECT_EQ(parse_imei("IMEI: 352653090190117"), "352653090190117");
    EXPECT_EQ(parse_imei("IMEI SV:  4"), "");       // not the IMEI line
    EXPECT_EQ(parse_imei("Model: WP7702"), "");

    EXPECT_EQ(parse_labeled("Model: WP7702", "Model"), "WP7702");
    EXPECT_EQ(parse_labeled("Revision: SWI9X06Y_02.32.02.00", "Revision"),
              "SWI9X06Y_02.32.02.00");
    EXPECT_EQ(parse_labeled("Manufacturer: Sierra", "Model"), "");

    EXPECT_EQ(model_capability("WP7702"), "LTE-M / NB-IoT / GSM");
    EXPECT_EQ(model_capability("EC25"), "LTE Cat-4 / 3G / 2G");
    EXPECT_EQ(model_capability("Telit"), "");
}

TEST(AtCgdcont, ExtractsApn) {
    EXPECT_EQ(parse_cgdcont("+CGDCONT: 1,\"IP\",\"airtelgprs.com\",\"0.0.0.0\",0,0"),
              "airtelgprs.com");
    EXPECT_EQ(parse_cgdcont("+CGDCONT: 1,\"IP\",\"\",\"0.0.0.0\",0,0"), "");  // undefined
    EXPECT_EQ(parse_cgdcont("OK"), "");
}

TEST(AtCgpaddr, ExtractsIp) {
    EXPECT_EQ(parse_cgpaddr("+CGPADDR: 1,\"10.181.22.7\""), "10.181.22.7");
    EXPECT_EQ(parse_cgpaddr("+CGPADDR: 1,100.92.3.44"), "100.92.3.44");
    EXPECT_EQ(parse_cgpaddr("+CGPADDR: 1,\"0.0.0.0\""), "");   // no context yet
    EXPECT_EQ(parse_cgpaddr("+CGPADDR: 1"), "");
}

TEST(AtCgcontrdp, ExtractsBothResolvers) {
    // Real WP7702 line (Airtel, airtelgprs.com). Note the empty <gw_addr> field.
    EXPECT_EQ(parse_cgcontrdp_dns(
                  "+CGCONTRDP: 1,5,airtelgprs.com,100.75.219.215,,117.96.122.74,59.144.127.117"),
              "117.96.122.74,59.144.127.117");
}

TEST(AtCgcontrdp, ExtractsPrimaryWhenSecondaryAbsent) {
    EXPECT_EQ(parse_cgcontrdp_dns("+CGCONTRDP: 1,5,internet,10.0.0.2,10.0.0.1,8.8.8.8"),
              "8.8.8.8");
    // Secondary present but empty.
    EXPECT_EQ(parse_cgcontrdp_dns("+CGCONTRDP: 1,5,internet,10.0.0.2,10.0.0.1,8.8.8.8,"),
              "8.8.8.8");
}

TEST(AtCgcontrdp, HandlesQuotingAndPadding) {
    EXPECT_EQ(parse_cgcontrdp_dns(
                  "+CGCONTRDP: 1,5,\"internet\",\"10.0.0.2\",\"10.0.0.1\", \"1.1.1.1\" , \"1.0.0.1\""),
              "1.1.1.1,1.0.0.1");
}

TEST(AtCgcontrdp, SkipsUnusableResolvers) {
    // 0.0.0.0 and malformed groups are dropped, not published.
    EXPECT_EQ(parse_cgcontrdp_dns("+CGCONTRDP: 1,5,internet,10.0.0.2,10.0.0.1,0.0.0.0,8.8.4.4"),
              "8.8.4.4");
    EXPECT_EQ(parse_cgcontrdp_dns("+CGCONTRDP: 1,5,internet,10.0.0.2,10.0.0.1,999.1.1.1"), "");
    EXPECT_EQ(parse_cgcontrdp_dns("+CGCONTRDP: 1,5,internet,10.0.0.2,10.0.0.1,10.0.0"), "");
    // IPv6 resolvers arrive as a 16-group dotted form — skipped, IPv4 only.
    EXPECT_EQ(parse_cgcontrdp_dns(
                  "+CGCONTRDP: 1,5,internet,10.0.0.2,10.0.0.1,"
                  "32.1.72.96.72.96.0.0.0.0.0.0.0.0.136.136"),
              "");
}

TEST(AtCgcontrdp, ToleratesShortAndEmptyLines) {
    EXPECT_EQ(parse_cgcontrdp_dns("+CGCONTRDP: 1,5,internet,10.0.0.2,10.0.0.1"), "");
    EXPECT_EQ(parse_cgcontrdp_dns("+CGCONTRDP:"), "");
    EXPECT_EQ(parse_cgcontrdp_dns("OK"), "");
}

TEST(AtIccid, AcceptsLongDigits) {
    EXPECT_EQ(parse_iccid("+QCCID: 8944500207123456789"), "8944500207123456789");
    EXPECT_EQ(parse_iccid("+CCID: 8901234567890123456F"), "8901234567890123456F");
    EXPECT_EQ(parse_iccid("+ICCID: 8944500207123456789"), "8944500207123456789"); // Sierra/std
    EXPECT_EQ(parse_iccid("8944500207123456789"), "8944500207123456789");  // bare
    EXPECT_EQ(parse_iccid("ERROR"), "");
}

TEST(Vendor, ClassifiesFromManufacturer) {
    EXPECT_EQ(parse_vendor("Sierra Wireless, Incorporated"), Vendor::Sierra);
    EXPECT_EQ(parse_vendor("Quectel"), Vendor::Quectel);
    EXPECT_EQ(parse_vendor("u-blox"), Vendor::UBlox);
    EXPECT_EQ(parse_vendor("Telit"), Vendor::Generic);
}

TEST(Vendor, ClassifiesFromModel) {
    EXPECT_EQ(parse_vendor("WP7702"), Vendor::Sierra);   // the board under test
    EXPECT_EQ(parse_vendor("HL7800"), Vendor::Sierra);
    EXPECT_EQ(parse_vendor("BG96"), Vendor::Quectel);
    EXPECT_EQ(parse_vendor("EC25"), Vendor::Quectel);
    EXPECT_EQ(parse_vendor("SARA-R410M"), Vendor::UBlox);
    EXPECT_EQ(parse_vendor("OK"), Vendor::Generic);      // not a model line
}

TEST(Vendor, IccidCommandPerVendor) {
    EXPECT_STREQ(iccid_command(Vendor::Quectel), "AT+QCCID");
    EXPECT_STREQ(iccid_command(Vendor::Sierra),  "AT+ICCID");
    EXPECT_STREQ(iccid_command(Vendor::UBlox),   "AT+CCID");
    EXPECT_STREQ(iccid_command(Vendor::Generic), "AT+CCID");
}

TEST(Vendor, GpsStartCommandsPerVendor) {
    const auto sierra = gps_start_commands(Vendor::Sierra);
    ASSERT_EQ(sierra.size(), 2u);
    EXPECT_EQ(sierra[0], "AT!ENTERCND=\"A710\"");
    EXPECT_EQ(sierra[1], "AT!GPSFIX=1,255,50");

    const auto quectel = gps_start_commands(Vendor::Quectel);
    ASSERT_EQ(quectel.size(), 1u);
    EXPECT_EQ(quectel[0], "AT+QGPS=1");

    EXPECT_TRUE(gps_start_commands(Vendor::Generic).empty());
}
