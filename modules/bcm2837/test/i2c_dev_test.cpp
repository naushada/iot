#ifndef __i2c_dev_test_cpp__
#define __i2c_dev_test_cpp__

// I2cDevTransport pulls in <linux/i2c.h>, so its tests only build on Linux.
// The argument-validation + not-open paths are exercised without any real I2C
// device (they return before the ioctl); the live transfer path is covered on
// hardware.
#if defined(__linux__)

#include <gtest/gtest.h>
#include "i2c_dev.hpp"

TEST(I2cDevTest, RejectsBadArguments) {
    I2cDevTransport bus;                 // not opened
    const std::uint8_t w[2] = {0x00, 0x11};
    std::uint8_t r[2] = {0, 0};
    EXPECT_EQ(bus.write(0x68, nullptr, 1),        I2cResult::BadArg);
    EXPECT_EQ(bus.write(0x68, w, 0),              I2cResult::BadArg);
    EXPECT_EQ(bus.write(0x80, w, 2),              I2cResult::BadArg);  // >7-bit
    EXPECT_EQ(bus.read(0x68, nullptr, 1),         I2cResult::BadArg);
    EXPECT_EQ(bus.write_read(0x68, w, 1, r, 0),   I2cResult::BadArg);
    EXPECT_EQ(bus.write_read(0x68, nullptr, 1, r, 2), I2cResult::BadArg);
}

TEST(I2cDevTest, NotOpenIsNotOk) {
    I2cDevTransport bus;
    const std::uint8_t w[1] = {0x00};
    std::uint8_t r[1] = {0};
    EXPECT_EQ(bus.write(0x68, w, 1), I2cResult::Timeout);  // valid args, no fd
    EXPECT_EQ(bus.read(0x68, r, 1),  I2cResult::Timeout);
    EXPECT_FALSE(bus.is_open());
}

TEST(I2cDevTest, OpenNonexistentDeviceFails) {
    I2cDevTransport bus;
    EXPECT_EQ(bus.open("/dev/i2c-does-not-exist"), -1);
    EXPECT_FALSE(bus.is_open());
}

TEST(I2cDevTest, ReadRegHelperRoutesThroughWriteRead) {
    // read_reg/write_reg are non-virtual helpers on the base; with no device
    // they must surface a non-Ok result rather than crash.
    I2cDevTransport bus;
    std::uint8_t out[2] = {0, 0};
    EXPECT_NE(bus.read_reg(0x68, 0x00, out, 2), I2cResult::Ok);
    EXPECT_NE(bus.write_reg(0x68, 0x7E, 0x11),  I2cResult::Ok);
}

#endif  // __linux__
#endif  /*__i2c_dev_test_cpp__*/
