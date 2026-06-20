#include <gtest/gtest.h>
#include <cmath>

#include "bme680.hpp"
#include "fake_i2c_transport.hpp"

namespace {
    /* A realistic-ish calibration set for the pure-function tests. */
    Bme680::Calib make_calib() {
        Bme680::Calib c{};
        c.t1 = 26000; c.t2 = 26400; c.t3 = 3;
        c.p1 = 36000; c.p2 = -10500; c.p3 = 88; c.p4 = 7000; c.p5 = -150;
        c.p6 = 30; c.p7 = 15; c.p8 = -2000; c.p9 = -2500; c.p10 = 30;
        c.h1 = 700; c.h2 = 1000; c.h3 = 0; c.h4 = 45; c.h5 = 20; c.h6 = 120; c.h7 = -100;
        return c;
    }
}

TEST(Bme680Test, Probe_Matches_ChipId) {
    FakeI2cTransport bus;
    Bme680 env(bus);
    bus.regs[0xD0] = Bme680::kChipId;
    EXPECT_TRUE(env.probe());
    bus.regs[0xD0] = 0x00;
    EXPECT_FALSE(env.probe());
}

TEST(Bme680Test, Init_Decodes_Calibration_Packing) {
    FakeI2cTransport bus;
    Bme680 env(bus);
    /* Temperature coeffs. */
    bus.regs[0xE9] = 0x34; bus.regs[0xEA] = 0x12;   // t1 = 0x1234
    bus.regs[0x8A] = 0x78; bus.regs[0x8B] = 0x56;   // t2 = 0x5678
    bus.regs[0x8C] = 0xFF;                          // t3 = -1
    bus.regs[0xA0] = 0x1E;                          // p10 = 30
    /* Humidity packed nibbles: H1=(E3<<4)|(E2&0xF), H2=(E1<<4)|(E2>>4). */
    bus.regs[0xE1] = 0x12; bus.regs[0xE2] = 0xCD; bus.regs[0xE3] = 0xAB;

    ASSERT_TRUE(env.init());
    const Bme680::Calib& c = env.calib();
    EXPECT_EQ(c.t1, 0x1234u);
    EXPECT_EQ(c.t2, 0x5678);
    EXPECT_EQ(c.t3, -1);
    EXPECT_EQ(c.p10, 30u);
    EXPECT_EQ(c.h1, 0xABDu);   // (0xAB<<4)|0x0D
    EXPECT_EQ(c.h2, 0x12Cu);   // (0x12<<4)|0x0C
}

TEST(Bme680Test, Init_Reports_Bus_Error) {
    FakeI2cTransport bus;
    Bme680 env(bus);
    bus.nack = true;
    EXPECT_FALSE(env.init());
}

TEST(Bme680Test, Temperature_Sets_TFine_And_Is_Monotonic) {
    Bme680::Calib a = make_calib();
    Bme680::Calib b = make_calib();
    const std::int32_t lo = Bme680::compensate_temperature(400000, a);
    const std::int32_t hi = Bme680::compensate_temperature(550000, b);
    EXPECT_NE(a.t_fine, 0);
    EXPECT_GT(hi, lo);                       // higher ADC -> higher temperature
    EXPECT_GT(lo, -4000);                    // within the -40..85 C envelope
    EXPECT_LT(hi, 8500);
}

TEST(Bme680Test, Pressure_Is_Deterministic_And_Guards_DivByZero) {
    Bme680::Calib c = make_calib();
    Bme680::compensate_temperature(500000, c);      // establish t_fine
    const std::int32_t p1 = Bme680::compensate_pressure(400000, c);
    const std::int32_t p2 = Bme680::compensate_pressure(400000, c);
    EXPECT_EQ(p1, p2);                               // pure / deterministic

    Bme680::Calib bad = make_calib();
    bad.p1 = 0;                                      // forces var1 == 0
    Bme680::compensate_temperature(500000, bad);
    EXPECT_EQ(Bme680::compensate_pressure(400000, bad), 0);
}

TEST(Bme680Test, Humidity_Is_Clamped_To_Range) {
    Bme680::Calib c = make_calib();
    Bme680::compensate_temperature(500000, c);
    const std::int32_t h = Bme680::compensate_humidity(25000, c);
    EXPECT_GE(h, 0);
    EXPECT_LE(h, 100000);                            // milli-%RH, clamped 0..100%
}

TEST(Bme680Test, Read_Triggers_Forced_Mode_And_Returns_Finite_Sample) {
    FakeI2cTransport bus;
    Bme680 env(bus);
    bus.regs[0xD0] = Bme680::kChipId;
    /* Seed a plausible calibration via the register file. */
    bus.regs[0xE9] = 0x90; bus.regs[0xEA] = 0x65;   // t1
    bus.regs[0x8A] = 0x20; bus.regs[0x8B] = 0x67;   // t2
    bus.regs[0x8E] = 0xA0; bus.regs[0x8F] = 0x8C;   // p1
    ASSERT_TRUE(env.init());

    /* field0 @ 0x1F: press[3], temp[3], hum[2]. */
    const std::uint8_t field[8] = {0x50,0x00,0x00, 0x7A,0x00,0x00, 0x61,0xA8};
    for (int i = 0; i < 8; ++i) bus.regs[0x1F + i] = field[i];

    Bme680::Sample s{};
    ASSERT_TRUE(env.read(s));
    /* ctrl_meas must have been written with forced mode (low 2 bits == 01). */
    EXPECT_EQ(bus.regs[0x74] & 0x03, 0x01);
    EXPECT_TRUE(std::isfinite(s.temperature_c));
    EXPECT_TRUE(std::isfinite(s.pressure_pa));
    EXPECT_TRUE(std::isfinite(s.humidity_pct));
}
