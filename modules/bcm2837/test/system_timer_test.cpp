#include "system_timer_test.hpp"

TEST_F(SystemTimerTest, NowUs_Composes_Chi_Clo) {
    poke(Reg::CHI, 0x2U);
    poke(Reg::CLO, 0x12345678U);
    EXPECT_EQ(m_timer.now_us(),
              (static_cast<std::uint64_t>(0x2U) << 32) | 0x12345678U);
    EXPECT_EQ(m_timer.now_lo(), 0x12345678U);
}

TEST_F(SystemTimerTest, NowUs_Low_Half_Only) {
    poke(Reg::CHI, 0U);
    poke(Reg::CLO, 0xDEADBEEFU);
    EXPECT_EQ(m_timer.now_us(), 0xDEADBEEFULL);
}

TEST_F(SystemTimerTest, Arm_Writes_The_Arm_Usable_Compare_Channels) {
    m_timer.arm(0xABCDU, /*ch=*/1);
    EXPECT_EQ(peek(Reg::C1), 0xABCDU);
    m_timer.arm(0x1111U, /*ch=*/3);
    EXPECT_EQ(peek(Reg::C3), 0x1111U);
    // GPU-owned channels are still addressable but not the default.
    m_timer.arm(0x2222U, /*ch=*/0);
    EXPECT_EQ(peek(Reg::C0), 0x2222U);
}

TEST_F(SystemTimerTest, Arm_Defaults_To_Channel_1) {
    m_timer.arm(0x5A5AU);                       // default kArmChannel == 1
    EXPECT_EQ(peek(Reg::C1), 0x5A5AU);
    EXPECT_EQ(SystemTimer::kArmChannel, 1U);
}

TEST_F(SystemTimerTest, Arm_Clamps_Out_Of_Range_Channel) {
    m_timer.arm(0x9999U, /*ch=*/7);             // clamps to channel 3
    EXPECT_EQ(peek(Reg::C3), 0x9999U);
}

TEST_F(SystemTimerTest, Matched_Reflects_CS_Match_Bits) {
    poke(Reg::CS, (1U << 3));                    // M3 set
    EXPECT_TRUE(m_timer.matched(3));
    EXPECT_FALSE(m_timer.matched(1));
}

TEST_F(SystemTimerTest, ClearMatch_Is_Write_One_To_Clear) {
    // clear_match writes the channel's bit mask (W1C on real silicon).
    m_timer.clear_match(1);
    EXPECT_EQ(peek(Reg::CS), (1U << 1));
    m_timer.clear_match(3);
    EXPECT_EQ(peek(Reg::CS), (1U << 3));
}
