#include <gtest/gtest.h>

#include "sms_receiver.hpp"

using namespace cellular;

// The canonical GSM7 "How are you?" DELIVER PDU (see sms_pdu_test.cpp) and two
// UCS2 concatenation parts of the same group (ref 0xA1, 2 parts): "AB" + "CD".
static const char* kPduHowAreYou =
    "07911326040000F0040B911346610089F60000208062917314080C"
    "C8F71D14969741F977FD07";
static const char* kPduConcat1 =   // ref A1, total 2, part 1, body "AB"
    "0044049121430008222030000000000A050003A1020100410042";
static const char* kPduConcat2 =   // ref A1, total 2, part 2, body "CD"
    "0044049121430008222030000000000A050003A1020200430044";

// +CMTI URC → read-back command, then header + PDU → a message and a delete.
TEST(SmsReceiver, CmtiReadThenDelete) {
    SmsReceiver rx;

    auto a = rx.on_line("+CMTI: \"SM\",3");
    ASSERT_EQ(a.commands.size(), 1u);
    EXPECT_EQ(a.commands[0], "AT+CMGR=3");
    EXPECT_TRUE(a.messages.empty());

    auto b = rx.on_line("+CMGR: 0,,23");       // reply header — nothing yet
    EXPECT_TRUE(b.commands.empty());
    EXPECT_TRUE(b.messages.empty());
    EXPECT_TRUE(rx.wants("anything"));         // now awaiting the PDU line

    auto c = rx.on_line(kPduHowAreYou);
    ASSERT_EQ(c.messages.size(), 1u);
    EXPECT_EQ(c.messages[0].sender, "+31641600986");
    EXPECT_EQ(c.messages[0].text, "How are you?");
    EXPECT_EQ(c.messages[0].index, 3);
    ASSERT_EQ(c.commands.size(), 1u);
    EXPECT_EQ(c.commands[0], "AT+CMGD=3");     // storage cleared after read
}

// Direct-deliver (+CMT, CNMI mode 2,2): no storage slot, so no delete.
TEST(SmsReceiver, DirectDeliverNoDelete) {
    SmsReceiver rx;
    EXPECT_TRUE(rx.on_line("+CMT: ,23").messages.empty());
    auto out = rx.on_line(kPduHowAreYou);
    ASSERT_EQ(out.messages.size(), 1u);
    EXPECT_EQ(out.messages[0].text, "How are you?");
    EXPECT_EQ(out.messages[0].index, -1);
    EXPECT_TRUE(out.commands.empty());
}

// Startup drain via +CMGL: the index is in the header, and we must NOT delete
// mid-listing (some modems renumber slots on delete).
TEST(SmsReceiver, CmglDrainNoDelete) {
    SmsReceiver rx;
    EXPECT_TRUE(rx.on_line("+CMGL: 2,1,,23").commands.empty());
    auto out = rx.on_line(kPduHowAreYou);
    ASSERT_EQ(out.messages.size(), 1u);
    EXPECT_TRUE(out.commands.empty());         // no AT+CMGD during a listing
}

// Concatenated SMS: no message surfaces until every part has arrived; then one
// reassembled message with the parts joined in order.
TEST(SmsReceiver, ConcatenatedReassembles) {
    SmsReceiver rx;

    rx.on_line("+CMTI: \"SM\",1");
    rx.on_line("+CMGR: 0,,10");
    auto p1 = rx.on_line(kPduConcat1);
    EXPECT_TRUE(p1.messages.empty());          // part 1 held for reassembly
    EXPECT_EQ(p1.commands.size(), 1u);         // still deleted from storage

    rx.on_line("+CMTI: \"SM\",2");
    rx.on_line("+CMGR: 0,,10");
    auto p2 = rx.on_line(kPduConcat2);
    ASSERT_EQ(p2.messages.size(), 1u);
    EXPECT_EQ(p2.messages[0].text, "ABCD");
    EXPECT_EQ(p2.messages[0].sender, "+1234");
}

// Out-of-order concat parts still reassemble.
TEST(SmsReceiver, ConcatenatedOutOfOrder) {
    SmsReceiver rx;
    rx.on_line("+CMGL: 5,1,,10");
    EXPECT_TRUE(rx.on_line(kPduConcat2).messages.empty());   // part 2 first
    rx.on_line("+CMGL: 4,1,,10");
    auto done = rx.on_line(kPduConcat1);
    ASSERT_EQ(done.messages.size(), 1u);
    EXPECT_EQ(done.messages[0].text, "ABCD");
}

TEST(SmsReceiver, WantsRoutesOnlySmsLines) {
    SmsReceiver rx;
    EXPECT_TRUE(rx.wants("+CMTI: \"SM\",1"));
    EXPECT_TRUE(rx.wants("+CMGR: 0,,23"));
    EXPECT_TRUE(rx.wants("+CMGL: 2,1,,23"));
    EXPECT_TRUE(rx.wants("+CMT: ,23"));
    EXPECT_FALSE(rx.wants("+CSQ: 20,0"));      // status line — not ours
    EXPECT_FALSE(rx.wants("OK"));
}

// A garbage PDU line after a header resets cleanly (no message, no crash).
TEST(SmsReceiver, BadPduResets) {
    SmsReceiver rx;
    rx.on_line("+CMTI: \"SM\",7");
    rx.on_line("+CMGR: 0,,23");
    auto out = rx.on_line("NOTHEX");
    EXPECT_TRUE(out.messages.empty());
    EXPECT_FALSE(rx.wants("OK"));              // back to Idle
}
