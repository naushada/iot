#include <gtest/gtest.h>
#include <cmath>

#include "opt3001.hpp"
#include "fake_i2c_transport.hpp"

TEST(Opt3001Test, Probe_Matches_DeviceId) {
    FakeI2cTransport bus;
    Opt3001 light(bus);
    /* Device-ID register 0x7F == 0x3001, MSB-first. */
    bus.regs[0x7F] = 0x30;
    bus.regs[0x80] = 0x01;   // (ptr auto-increments to 0x80 for the 2nd byte)
    EXPECT_TRUE(light.probe());

    bus.regs[0x7F] = 0x00;
    EXPECT_FALSE(light.probe());
}

TEST(Opt3001Test, Init_Writes_Continuous_Config_BigEndian) {
    FakeI2cTransport bus;
    Opt3001 light(bus);
    EXPECT_EQ(light.init(), I2cResult::Ok);
    EXPECT_EQ(bus.regs[0x01], 0xCE);   // config MSB
    EXPECT_EQ(bus.regs[0x02], 0x10);   // config LSB
}

TEST(Opt3001Test, ReadLux_Decodes_Exponent_Mantissa) {
    FakeI2cTransport bus;
    Opt3001 light(bus);
    /* Result 0x4199: E=4, mantissa=0x199(409) -> 0.01 * 16 * 409 = 65.44 lux. */
    bus.regs[0x00] = 0x41;
    bus.regs[0x01] = 0x99;
    double lux = 0.0;
    ASSERT_TRUE(light.read_lux(lux));
    EXPECT_NEAR(lux, 65.44, 1e-9);
}

TEST(Opt3001Test, ReadLux_Zero) {
    FakeI2cTransport bus;
    Opt3001 light(bus);
    bus.regs[0x00] = 0x00;
    bus.regs[0x01] = 0x00;
    double lux = -1.0;
    ASSERT_TRUE(light.read_lux(lux));
    EXPECT_DOUBLE_EQ(lux, 0.0);
}

TEST(Opt3001Test, ReadLux_Reports_Bus_Error) {
    FakeI2cTransport bus;
    Opt3001 light(bus);
    bus.nack = true;
    double lux = 0.0;
    EXPECT_FALSE(light.read_lux(lux));
}
