#ifndef __i2c_bus_test_cpp__
#define __i2c_bus_test_cpp__

#include "i2c_bus_test.hpp"

/* ---- bus_init: pin-mux + clock + enable ---------------------------------- */

TEST_F(I2cBusTest, BusInit_Muxes_Gpio2_3_Alt0_Sets_Divider_And_Enables) {
    Bcm2837I2cTransport bus(i2c(), gpio(), /*divider=*/2500);
    bus.bus_init();

    /* GPIO2 field = GPFSEL0[8:6], GPIO3 field = GPFSEL0[11:9]; ALT0 == 0b100. */
    EXPECT_EQ((gpfsel0() >> 6) & 0b111U, 0b100U);
    EXPECT_EQ((gpfsel0() >> 9) & 0b111U, 0b100U);
    EXPECT_EQ(i2c().clock_divider(), 2500U);
    EXPECT_EQ(i2c().get_control(BCM2837::BSCRegistersAddress::Control::I2CEN), 1U);
}

/* ---- argument validation ------------------------------------------------- */

TEST_F(I2cBusTest, Write_Rejects_Bad_Arguments) {
    Bcm2837I2cTransport bus(i2c(), gpio());
    const std::uint8_t b[1] = {0x00};
    EXPECT_EQ(bus.write(0x68, nullptr, 1), I2cResult::BadArg);
    EXPECT_EQ(bus.write(0x68, b, 0),       I2cResult::BadArg);
    EXPECT_EQ(bus.write(0x80, b, 1),       I2cResult::BadArg); // addr > 7-bit
}

TEST_F(I2cBusTest, Read_Rejects_Bad_Arguments) {
    Bcm2837I2cTransport bus(i2c(), gpio());
    std::uint8_t b[1] = {0};
    EXPECT_EQ(bus.read(0x68, nullptr, 1), I2cResult::BadArg);
    EXPECT_EQ(bus.read(0x68, b, 0),       I2cResult::BadArg);
    EXPECT_EQ(bus.read(0x80, b, 1),       I2cResult::BadArg);
}

/* ---- transfer programming + timeout -------------------------------------- */

TEST_F(I2cBusTest, Write_Programs_Address_Length_Control_Then_Times_Out) {
    /* Empty script -> poll_status() returns 0 forever (DONE never asserts). */
    ScriptedTransport bus(i2c(), gpio(), /*divider=*/2500, /*max_spins=*/8);
    const std::uint8_t payload[2] = {0xAB, 0xCD};

    EXPECT_EQ(bus.write(0x68, payload, sizeof(payload)), I2cResult::Timeout);

    /* prime() + start_write() side effects are observable on the buffer. */
    EXPECT_EQ(i2c().slave_address(), 0x68U);
    EXPECT_EQ(i2c().data_length(),   2U);
    EXPECT_EQ(i2c().get_control(BCM2837::BSCRegistersAddress::Control::READ), 0U);
    EXPECT_EQ(i2c().get_control(BCM2837::BSCRegistersAddress::Control::ST),   1U);
}

TEST_F(I2cBusTest, Read_Programs_Read_Bit_Then_Times_Out) {
    ScriptedTransport bus(i2c(), gpio(), /*divider=*/2500, /*max_spins=*/8);
    std::uint8_t buf[2] = {0, 0};

    EXPECT_EQ(bus.read(0x76, buf, sizeof(buf)), I2cResult::Timeout);

    EXPECT_EQ(i2c().slave_address(), 0x76U);
    EXPECT_EQ(i2c().data_length(),   2U);
    EXPECT_EQ(i2c().get_control(BCM2837::BSCRegistersAddress::Control::READ), 1U);
    EXPECT_EQ(i2c().get_control(BCM2837::BSCRegistersAddress::Control::ST),   1U);
}

/* ---- error classification ------------------------------------------------ */

TEST_F(I2cBusTest, Write_Reports_Nack_On_Err) {
    ScriptedTransport bus(i2c(), gpio());
    bus.script = {ERR};
    const std::uint8_t b[1] = {0x00};
    EXPECT_EQ(bus.write(0x68, b, 1), I2cResult::Nack);
}

TEST_F(I2cBusTest, Read_Reports_ClockTimeout_On_Clkt) {
    ScriptedTransport bus(i2c(), gpio());
    bus.script = {CLKT};
    std::uint8_t b[1] = {0};
    EXPECT_EQ(bus.read(0x68, b, 1), I2cResult::ClockTimeout);
}

/* ---- happy paths --------------------------------------------------------- */

TEST_F(I2cBusTest, Write_Feeds_Fifo_Until_Done) {
    ScriptedTransport bus(i2c(), gpio());
    bus.script = {TXD, TXD, DONE};       // feed byte0, feed byte1, complete
    const std::uint8_t payload[2] = {0x11, 0x22};

    EXPECT_EQ(bus.write(0x68, payload, sizeof(payload)), I2cResult::Ok);
    /* Last byte pushed into the FIFO is observable on the buffer. */
    EXPECT_EQ(i2c().read_byte(), 0x22U);
}

TEST_F(I2cBusTest, Read_Drains_Fifo_Until_Done) {
    ScriptedTransport bus(i2c(), gpio());
    bus.script = {RXD, RXD, DONE};       // one byte per RXD, then complete
    bus.rx     = {0xDE, 0xAD};
    std::uint8_t buf[2] = {0, 0};

    EXPECT_EQ(bus.read(0x76, buf, sizeof(buf)), I2cResult::Ok);
    EXPECT_EQ(buf[0], 0xDEU);
    EXPECT_EQ(buf[1], 0xADU);
}

TEST_F(I2cBusTest, Read_Short_Read_Is_Timeout) {
    /* DONE arrives with a byte still owed and nothing left in the FIFO. */
    ScriptedTransport bus(i2c(), gpio());
    bus.script = {RXD, DONE};
    bus.rx     = {0xDE};
    std::uint8_t buf[2] = {0, 0};

    EXPECT_EQ(bus.read(0x76, buf, sizeof(buf)), I2cResult::Timeout);
    EXPECT_EQ(buf[0], 0xDEU);             // the byte we did get is kept
}

/* ---- write_read (register read primitive) -------------------------------- */

TEST_F(I2cBusTest, WriteRead_Writes_Pointer_Then_Reads) {
    ScriptedTransport bus(i2c(), gpio());
    /* write phase: feed 1 reg byte + DONE; read phase: 2 bytes + DONE. */
    bus.script = {TXD, DONE, RXD, RXD, DONE};
    bus.rx     = {0xD1, 0x00};            // e.g. BMI160 CHIP_ID read-back
    const std::uint8_t reg = 0x00;
    std::uint8_t out[2] = {0, 0};

    EXPECT_EQ(bus.write_read(0x68, &reg, 1, out, sizeof(out)), I2cResult::Ok);
    EXPECT_EQ(out[0], 0xD1U);
    EXPECT_EQ(out[1], 0x00U);
}

TEST_F(I2cBusTest, WriteRead_Short_Circuits_On_Write_Nack) {
    ScriptedTransport bus(i2c(), gpio());
    bus.script = {ERR};                  // write phase NACKs immediately
    const std::uint8_t reg = 0x00;
    std::uint8_t out[2] = {0xFF, 0xFF};

    EXPECT_EQ(bus.write_read(0x68, &reg, 1, out, sizeof(out)), I2cResult::Nack);
    EXPECT_EQ(out[0], 0xFFU);            // read phase never ran
}

#endif /*__i2c_bus_test_cpp__*/
