#include <gtest/gtest.h>

#include "bmi160.hpp"
#include "fake_i2c_transport.hpp"

TEST(Bmi160Test, Probe_Matches_ChipId) {
    FakeI2cTransport bus;
    Bmi160 imu(bus);
    bus.regs[0x00] = Bmi160::kChipId;          // CHIP_ID
    EXPECT_TRUE(imu.probe());

    bus.regs[0x00] = 0x00;
    EXPECT_FALSE(imu.probe());
}

TEST(Bmi160Test, Probe_Honours_Address) {
    FakeI2cTransport bus;
    Bmi160 imu(bus, Bmi160::kAddrSecondary);
    bus.regs[0x00] = Bmi160::kChipId;
    EXPECT_TRUE(imu.probe());
    EXPECT_EQ(bus.lastAddr, Bmi160::kAddrSecondary);
}

TEST(Bmi160Test, Init_Issues_Accel_Then_Gyro_Power_Commands) {
    FakeI2cTransport bus;
    Bmi160 imu(bus);
    EXPECT_EQ(imu.init(), I2cResult::Ok);
    /* Last write wins on the CMD register; gyro command is issued second. */
    EXPECT_EQ(bus.regs[0x7E], 0x15);
}

TEST(Bmi160Test, Read_Decodes_Signed_LittleEndian_Axes) {
    FakeI2cTransport bus;
    Bmi160 imu(bus);
    /* Data block at 0x0C: gyro X/Y/Z then accel X/Y/Z, LSB first. */
    const std::uint8_t blk[12] = {
        0x01, 0x00,   // gx = +1
        0xFF, 0xFF,   // gy = -1
        0x00, 0x80,   // gz = -32768
        0xFF, 0x7F,   // ax = +32767
        0x34, 0x12,   // ay = 0x1234
        0x00, 0x00,   // az = 0
    };
    for (int i = 0; i < 12; ++i) bus.regs[0x0C + i] = blk[i];

    Bmi160::Sample s{};
    ASSERT_TRUE(imu.read(s));
    EXPECT_EQ(s.gx, 1);
    EXPECT_EQ(s.gy, -1);
    EXPECT_EQ(s.gz, -32768);
    EXPECT_EQ(s.ax, 32767);
    EXPECT_EQ(s.ay, 0x1234);
    EXPECT_EQ(s.az, 0);
}

TEST(Bmi160Test, Read_Reports_Bus_Error) {
    FakeI2cTransport bus;
    Bmi160 imu(bus);
    bus.nack = true;
    Bmi160::Sample s{};
    EXPECT_FALSE(imu.read(s));
}
