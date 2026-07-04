#include <gtest/gtest.h>

#include "sms_pdu.hpp"

using namespace cellular;

// Canonical 3GPP example: SMS-DELIVER, GSM 7-bit body "How are you?" from an
// international number. Exercises SMSC skip, semi-octet TP-OA, SCTS and 7-bit
// unpack in one go.
TEST(SmsPdu, Gsm7Canonical) {
    SmsMessage m;
    ASSERT_TRUE(decode_sms_deliver(
        "07911326040000F0040B911346610089F60000208062917314080C"
        "C8F71D14969741F977FD07", m));
    EXPECT_EQ(m.sender, "+31641600986");
    EXPECT_EQ(m.text, "How are you?");
    EXPECT_EQ(m.scts, "2002-08-26T19:37:41");
    EXPECT_EQ(m.total, 0);          // single-part
}

// UCS2 (TP-DCS 0x08) body → UTF-8.
TEST(SmsPdu, Ucs2Body) {
    SmsMessage m;
    ASSERT_TRUE(decode_sms_deliver(
        "0004049121430008222030000000000400680069", m));
    EXPECT_EQ(m.sender, "+1234");
    EXPECT_EQ(m.text, "hi");
}

// GSM 7-bit escape → extension table (euro sign, U+20AC).
TEST(SmsPdu, Gsm7Extension) {
    SmsMessage m;
    ASSERT_TRUE(decode_sms_deliver(
        "000404912143000022203000000000029B32", m));
    EXPECT_EQ(m.text, "\xE2\x82\xAC");   // €
}

// Alphanumeric originating address (TON 5): the address octets are 7-bit packed
// text ("Info"), and the body is GSM7 "Hi".
TEST(SmsPdu, AlphanumericSender) {
    SmsMessage m;
    ASSERT_TRUE(decode_sms_deliver(
        "000408D049B7F90D00002220300000000002C834", m));
    EXPECT_EQ(m.sender, "Info");
    EXPECT_EQ(m.text, "Hi");
}

// Concatenated (multipart) SMS: TP-UDHI set, 8-bit concat IE (ref/total/part),
// UCS2 body fragment. The UDH is parsed and the body starts after it.
TEST(SmsPdu, ConcatenatedUcs2Part) {
    SmsMessage m;
    ASSERT_TRUE(decode_sms_deliver(
        "0044049121430008222030000000000A050003A1020100410042", m));
    EXPECT_EQ(m.sender, "+1234");
    EXPECT_EQ(m.ref, 0xA1);
    EXPECT_EQ(m.total, 2);
    EXPECT_EQ(m.part, 1);
    EXPECT_EQ(m.text, "AB");
}

TEST(SmsPdu, RejectsMalformed) {
    SmsMessage m;
    EXPECT_FALSE(decode_sms_deliver("", m));              // empty
    EXPECT_FALSE(decode_sms_deliver("ABC", m));           // odd length
    EXPECT_FALSE(decode_sms_deliver("ZZ", m));            // non-hex
    EXPECT_FALSE(decode_sms_deliver("07", m));            // SMSC len 7, truncated
    EXPECT_FALSE(decode_sms_deliver("00", m));            // no TPDU octet
    EXPECT_FALSE(decode_sms_deliver("0001", m));          // TP-MTI=01 → not DELIVER
}
