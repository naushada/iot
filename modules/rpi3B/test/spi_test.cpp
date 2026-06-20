#ifndef __spi_test_cpp__
#define __spi_test_cpp__

#include "spi_test.hpp"

void SPITest::SetUp() {}
void SPITest::TearDown() {}
void SPITest::TestBody() {}

TEST_F(SPITest, CS_Field_SetClrGet) {
    spi().set_cs(RPi3B::SPIRegistersAddress::ControlStatus::TA, 1);
    EXPECT_EQ(spi().get_cs(RPi3B::SPIRegistersAddress::ControlStatus::TA), 1U);
    spi().clr_cs(RPi3B::SPIRegistersAddress::ControlStatus::TA);
    EXPECT_EQ(spi().get_cs(RPi3B::SPIRegistersAddress::ControlStatus::TA), 0U);
}

TEST_F(SPITest, ChipSelect_TwoBitField) {
    spi().chip_select(2);
    EXPECT_EQ(spi().chip_select(), 2U);
    /* value must be confined to bits[1:0] */
    spi().chip_select(0b11);
    EXPECT_EQ(spi().chip_select(), 0b11U);
}

TEST_F(SPITest, Mode_Sets_CPOL_CPHA) {
    spi().mode(1, 1);
    EXPECT_EQ(spi().get_cs(RPi3B::SPIRegistersAddress::ControlStatus::CPOL), 1U);
    EXPECT_EQ(spi().get_cs(RPi3B::SPIRegistersAddress::ControlStatus::CPHA), 1U);
}

TEST_F(SPITest, Begin_End_Transfer) {
    spi().begin_transfer();
    EXPECT_EQ(spi().get_cs(RPi3B::SPIRegistersAddress::ControlStatus::TA), 1U);
    spi().end_transfer();
    EXPECT_EQ(spi().get_cs(RPi3B::SPIRegistersAddress::ControlStatus::TA), 0U);
}

TEST_F(SPITest, Clear_FIFO_SetsBothBits) {
    spi().clear_fifo();
    EXPECT_EQ(spi().get_cs(RPi3B::SPIRegistersAddress::ControlStatus::CLEAR), 0b11U);
}

TEST_F(SPITest, High_Bit_Field_CSPOL2) {
    /* CSPOL2 lives at bit23 -- exercises high-bit field math. */
    spi().set_cs(RPi3B::SPIRegistersAddress::ControlStatus::CSPOL2, 1);
    EXPECT_EQ(spi().get_cs(RPi3B::SPIRegistersAddress::ControlStatus::CSPOL2), 1U);
    /* Setting a high bit must not bleed into CS_LINE at bits[1:0]. */
    EXPECT_EQ(spi().chip_select(), 0U);
}

TEST_F(SPITest, ClockDivider) {
    spi().clock_divider(256);
    EXPECT_EQ(spi().clock_divider(), 256U);
}

TEST_F(SPITest, FIFO_RoundTrip) {
    spi().write_byte(0x3C);
    EXPECT_EQ(spi().read_byte(), 0x3CU);
}

TEST_F(SPITest, Status_Helpers_DefaultZero) {
    EXPECT_FALSE(spi().is_done());
    EXPECT_FALSE(spi().can_write());
    EXPECT_FALSE(spi().can_read());
}

#endif /*__spi_test_cpp__*/
