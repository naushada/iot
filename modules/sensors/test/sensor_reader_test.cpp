#include <gtest/gtest.h>
#include <algorithm>

#include "sensor_reader.hpp"
#include "fake_i2c_transport.hpp"

namespace {
    bool has_key(const std::vector<sensors::KV>& kv, const std::string& k) {
        return std::any_of(kv.begin(), kv.end(),
                           [&](const sensors::KV& e) { return e.key == k; });
    }
    std::string val(const std::vector<sensors::KV>& kv, const std::string& k) {
        for (auto& e : kv) if (e.key == k) return e.value;
        return {};
    }
}

TEST(SensorReader, EmptyCache_Yields_No_Keys) {
    sensors::SensorCache cache;
    EXPECT_TRUE(cache.to_kv().empty());
}

TEST(SensorReader, Env_Lux_Imu_Produce_Expected_Keys) {
    sensors::SensorCache cache;
    cache.set_env(23.5, 101325.0, 48.0);
    cache.set_lux(65.44);
    cache.set_imu(1, -2, 3, -4, 5, -6);

    auto kv = cache.to_kv();
    EXPECT_EQ(val(kv, "iot.sensor.temp"), "23.50");
    EXPECT_EQ(val(kv, "iot.sensor.humidity"), "48.00");
    EXPECT_EQ(val(kv, "iot.sensor.pressure"), "101325.0");
    EXPECT_EQ(val(kv, "iot.sensor.lux"), "65.44");
    EXPECT_EQ(val(kv, "iot.sensor.accel"), "1,-2,3");
    EXPECT_EQ(val(kv, "iot.sensor.gyro"), "-4,5,-6");
    EXPECT_TRUE(has_key(kv, "iot.sensor.version"));
}

TEST(SensorReader, Version_Bumps_Per_Update) {
    sensors::SensorCache cache;
    cache.set_lux(1.0);
    const std::string v1 = val(cache.to_kv(), "iot.sensor.version");
    cache.set_lux(2.0);
    const std::string v2 = val(cache.to_kv(), "iot.sensor.version");
    EXPECT_NE(v1, v2);
}

TEST(SensorReader, SampleAll_Reads_All_Three_Chips) {
    sensors::SensorCache cache;
    FakeI2cTransport bus;
    bus.regs[0xD0] = Bme680::kChipId;             // BME680 present
    bus.regs[0x00] = Bmi160::kChipId;             // BMI160 chip-id @ 0x00
    bus.regs[0x7F] = 0x30; bus.regs[0x80] = 0x01; // OPT3001 device-id 0x3001
    for (int i = 0; i < 12; ++i) bus.regs[0x0C + i] = static_cast<std::uint8_t>(i + 1);

    sensors::SampleResult r = sensors::sample_all(bus, cache);
    EXPECT_TRUE(r.bme);
    EXPECT_TRUE(r.imu);
    EXPECT_TRUE(r.light);
    EXPECT_TRUE(r.any());

    auto kv = cache.to_kv();
    EXPECT_TRUE(has_key(kv, "iot.sensor.temp"));
    EXPECT_TRUE(has_key(kv, "iot.sensor.accel"));
    EXPECT_TRUE(has_key(kv, "iot.sensor.lux"));
}

TEST(SensorReader, SampleAll_Skips_Absent_Chips) {
    sensors::SensorCache cache;
    FakeI2cTransport bus;            // empty register file: nothing probes
    sensors::SampleResult r = sensors::sample_all(bus, cache);
    EXPECT_FALSE(r.any());
    EXPECT_TRUE(cache.to_kv().empty());
}
