#ifndef __i2c_test_cpp__
#define __i2c_test_cpp__

#include "i2c_test.hpp"

void I2CTest::SetUp() {}
void I2CTest::TearDown() {}
void I2CTest::TestBody() {}

TEST_F(I2CTest, Enable_Disable) {
    i2c().enable();
    EXPECT_EQ(i2c().get_control(RPi3B::BSCRegistersAddress::Control::I2CEN), 1U);
    i2c().disable();
    EXPECT_EQ(i2c().get_control(RPi3B::BSCRegistersAddress::Control::I2CEN), 0U);
}

TEST_F(I2CTest, Control_Field_SetClrGet) {
    i2c().set_control(RPi3B::BSCRegistersAddress::Control::INTR, 1);
    EXPECT_EQ(i2c().get_control(RPi3B::BSCRegistersAddress::Control::INTR), 1U);
    i2c().clr_control(RPi3B::BSCRegistersAddress::Control::INTR);
    EXPECT_EQ(i2c().get_control(RPi3B::BSCRegistersAddress::Control::INTR), 0U);
}

TEST_F(I2CTest, Control_Multibit_CLEAR_Field) {
    /* CLEAR is a 2-bit field at bits[5:4]. */
    i2c().set_control(RPi3B::BSCRegistersAddress::Control::CLEAR, 0b10);
    EXPECT_EQ(i2c().get_control(RPi3B::BSCRegistersAddress::Control::CLEAR), 0b10U);
}

TEST_F(I2CTest, Control_Fields_Are_Independent) {
    /* Setting one field must not disturb another (read/modify/write). */
    i2c().set_control(RPi3B::BSCRegistersAddress::Control::I2CEN, 1);
    i2c().set_control(RPi3B::BSCRegistersAddress::Control::READ, 1);
    EXPECT_EQ(i2c().get_control(RPi3B::BSCRegistersAddress::Control::I2CEN), 1U);
    EXPECT_EQ(i2c().get_control(RPi3B::BSCRegistersAddress::Control::READ), 1U);
    i2c().clr_control(RPi3B::BSCRegistersAddress::Control::READ);
    EXPECT_EQ(i2c().get_control(RPi3B::BSCRegistersAddress::Control::I2CEN), 1U);
    EXPECT_EQ(i2c().get_control(RPi3B::BSCRegistersAddress::Control::READ), 0U);
}

TEST_F(I2CTest, Start_Read_Sets_READ_ST_EN) {
    i2c().start_read();
    EXPECT_EQ(i2c().get_control(RPi3B::BSCRegistersAddress::Control::I2CEN), 1U);
    EXPECT_EQ(i2c().get_control(RPi3B::BSCRegistersAddress::Control::READ), 1U);
    EXPECT_EQ(i2c().get_control(RPi3B::BSCRegistersAddress::Control::ST), 1U);
}

TEST_F(I2CTest, Start_Write_Clears_READ) {
    i2c().start_write();
    EXPECT_EQ(i2c().get_control(RPi3B::BSCRegistersAddress::Control::READ), 0U);
    EXPECT_EQ(i2c().get_control(RPi3B::BSCRegistersAddress::Control::ST), 1U);
}

TEST_F(I2CTest, Slave_Address_Masked_To_7bits) {
    i2c().slave_address(0x1FF);
    EXPECT_EQ(i2c().slave_address(), 0x7FU);
}

TEST_F(I2CTest, DataLength_And_ClockDivider) {
    i2c().data_length(34);
    EXPECT_EQ(i2c().data_length(), 34U);
    i2c().clock_divider(2500);
    EXPECT_EQ(i2c().clock_divider(), 2500U);
}

TEST_F(I2CTest, FIFO_RoundTrip) {
    i2c().write_byte(0xA5);
    EXPECT_EQ(i2c().read_byte(), 0xA5U);
}

TEST_F(I2CTest, Status_Read) {
    /* With a zeroed buffer all status bits read 0. */
    EXPECT_EQ(i2c().get_status(RPi3B::BSCRegistersAddress::Status::DONE), 0U);
    EXPECT_EQ(i2c().get_status(RPi3B::BSCRegistersAddress::Status::TA), 0U);
}

#endif /*__i2c_test_cpp__*/
