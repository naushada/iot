#ifndef __i2c_irq_test_cpp__
#define __i2c_irq_test_cpp__

#include "i2c_irq_test.hpp"

namespace {
    struct CbRec { I2cResult r = I2cResult::Timeout; int n = 0; };
    void rec_cb(I2cResult r, void* user) {
        auto* c = static_cast<CbRec*>(user);
        c->r = r; c->n++;
    }
}

/* ---- P4: bus_init wires pin-mux + clock + the BSC IRQ ---- */

TEST_F(I2cIrqTest, BusInit_Muxes_Clocks_Enables_And_Arms_Irq) {
    ScriptedIrqTransport bus(i2c(), gpio(), irq(), /*divider=*/2500);
    bus.bus_init();
    EXPECT_EQ((gpfsel0() >> 6) & 0b111U, 0b100U);   // GPIO2 → ALT0
    EXPECT_EQ((gpfsel0() >> 9) & 0b111U, 0b100U);   // GPIO3 → ALT0
    EXPECT_EQ(i2c().clock_divider(), 2500U);
    EXPECT_EQ(i2c().get_control(BCM2837::BSCRegistersAddress::Control::I2CEN), 1U);
    EXPECT_TRUE(irq().isEnabled(Bcm2837I2cIrqTransport::kBsc1Irq));  // IRQ 53 armed
}

/* ---- argument validation ---- */

TEST_F(I2cIrqTest, Rejects_Bad_Arguments) {
    ScriptedIrqTransport bus(i2c(), gpio(), irq());
    const std::uint8_t b[1] = {0};
    std::uint8_t r[1] = {0};
    EXPECT_EQ(bus.write(0x68, nullptr, 1), I2cResult::BadArg);
    EXPECT_EQ(bus.write(0x68, b, 0),       I2cResult::BadArg);
    EXPECT_EQ(bus.write(0x80, b, 1),       I2cResult::BadArg);
    EXPECT_EQ(bus.read(0x68, nullptr, 1),  I2cResult::BadArg);
}

/* ---- P1: small-transfer DONE path ---- */

TEST_F(I2cIrqTest, Write_Completes_On_Done) {
    ScriptedIrqTransport bus(i2c(), gpio(), irq());
    bus.script = {TXD, TXD, DONE};
    const std::uint8_t payload[2] = {0x11, 0x22};

    EXPECT_EQ(bus.write(0x68, payload, sizeof(payload)), I2cResult::Ok);
    EXPECT_EQ(i2c().slave_address(), 0x68U);
    EXPECT_EQ(i2c().data_length(),   2U);
    EXPECT_EQ(i2c().get_control(BCM2837::BSCRegistersAddress::Control::READ), 0U);
    EXPECT_EQ(i2c().read_byte(), 0x22U);   // last byte the ISR pushed
}

TEST_F(I2cIrqTest, Read_Completes_On_Done) {
    ScriptedIrqTransport bus(i2c(), gpio(), irq());
    bus.script = {RXD, RXD, DONE};
    bus.rx     = {0xDE, 0xAD};
    std::uint8_t buf[2] = {0, 0};

    EXPECT_EQ(bus.read(0x76, buf, sizeof(buf)), I2cResult::Ok);
    EXPECT_EQ(buf[0], 0xDEU);
    EXPECT_EQ(buf[1], 0xADU);
    EXPECT_EQ(i2c().get_control(BCM2837::BSCRegistersAddress::Control::READ), 1U);
}

/* ---- P2: larger transfer — ISR drains/fills across several interrupts ---- */

TEST_F(I2cIrqTest, Read_Drains_Across_Multiple_Interrupts) {
    ScriptedIrqTransport bus(i2c(), gpio(), irq());
    // Each "interrupt" delivers one byte then yields (a 0 word), then DONE.
    bus.script = {RXD, 0, RXD, 0, RXD, DONE};
    bus.rx     = {0x01, 0x02, 0x03};
    std::uint8_t buf[3] = {0, 0, 0};

    EXPECT_EQ(bus.read(0x50, buf, sizeof(buf)), I2cResult::Ok);
    EXPECT_EQ(buf[0], 0x01U);
    EXPECT_EQ(buf[1], 0x02U);
    EXPECT_EQ(buf[2], 0x03U);
}

/* ---- error classification ---- */

TEST_F(I2cIrqTest, Reports_Nack_On_Err) {
    ScriptedIrqTransport bus(i2c(), gpio(), irq());
    bus.script = {ERR};
    const std::uint8_t b[1] = {0};
    EXPECT_EQ(bus.write(0x68, b, 1), I2cResult::Nack);
}

TEST_F(I2cIrqTest, Reports_ClockTimeout_On_Clkt) {
    ScriptedIrqTransport bus(i2c(), gpio(), irq());
    bus.script = {CLKT};
    std::uint8_t b[1] = {0};
    EXPECT_EQ(bus.read(0x68, b, 1), I2cResult::ClockTimeout);
}

TEST_F(I2cIrqTest, Short_Read_Is_Timeout) {
    ScriptedIrqTransport bus(i2c(), gpio(), irq());
    bus.script = {RXD, DONE};   // DONE with a byte still owed
    bus.rx     = {0xDE};
    std::uint8_t buf[2] = {0, 0};
    EXPECT_EQ(bus.read(0x76, buf, sizeof(buf)), I2cResult::Timeout);
    EXPECT_EQ(buf[0], 0xDEU);
}

/* ---- P3: watchdog ---- */

TEST_F(I2cIrqTest, Watchdog_Times_Out_When_No_Completion) {
    // Empty script → read_status() returns 0 forever; DONE never arrives.
    ScriptedIrqTransport bus(i2c(), gpio(), irq(), /*divider=*/2500, /*max_waits=*/5);
    const std::uint8_t b[1] = {0};
    EXPECT_EQ(bus.write(0x68, b, 1), I2cResult::Timeout);
}

TEST_F(I2cIrqTest, Watchdog_Times_Out_On_Microsecond_Deadline) {
    // Huge iteration cap so ONLY the µs deadline can stop the wait. The clock
    // advances 400 µs per read; with a 1 ms deadline the watchdog fires after a
    // few iterations — far below the 1,000,000 iteration backstop.
    ScriptedIrqTransport bus(i2c(), gpio(), irq(), /*divider=*/2500,
                             /*max_waits=*/1000000U, /*timeout_us=*/1000U);
    bus.nowStep = 400;                       // 400 µs of "wall clock" per now_us()
    const std::uint8_t b[1] = {0};
    EXPECT_EQ(bus.write(0x68, b, 1), I2cResult::Timeout);
    EXPECT_LT(bus.nowVal, 1000000ULL);       // stopped on time, not the iter cap
}

TEST_F(I2cIrqTest, Time_Source_Feeds_The_Watchdog_Clock) {
    // set_time_source() wires a SystemTimer; the default now_us() then reports
    // that timer's 64-bit (CHI:CLO) counter. Without a source it reads 0.
    std::vector<std::uint32_t> treg(
        BCM2837::SystemTimerRegistersAddress::Register::ST_MAX);
    SystemTimer timer(treg.data());
    treg[BCM2837::SystemTimerRegistersAddress::Register::CLO] = 0x12340000U;
    treg[BCM2837::SystemTimerRegistersAddress::Register::CHI] = 0x2U;

    ProbeIrqTransport bus(i2c(), gpio(), irq());
    EXPECT_EQ(bus.probe_now_us(), 0ULL);     // no source yet → "no clock"
    bus.set_time_source(&timer);
    EXPECT_EQ(bus.probe_now_us(),
              (static_cast<std::uint64_t>(0x2U) << 32) | 0x12340000U);
}

/* ---- P5: async (kick + ISR-fired callback) ---- */

TEST_F(I2cIrqTest, Async_Write_Fires_Callback_On_Completion) {
    ScriptedIrqTransport bus(i2c(), gpio(), irq());
    bus.script = {TXD, TXD, DONE};
    const std::uint8_t payload[2] = {0xAA, 0xBB};
    CbRec rec;

    EXPECT_EQ(bus.write_async(0x68, payload, sizeof(payload), rec_cb, &rec), I2cResult::Ok);
    EXPECT_EQ(rec.n, 0);                       // not finished yet — in flight
    // A second transfer while one is active is rejected.
    EXPECT_EQ(bus.write_async(0x69, payload, 1, rec_cb, &rec), I2cResult::BadArg);

    bus.handle_irq();                          // simulate the interrupt firing
    EXPECT_EQ(rec.n, 1);
    EXPECT_EQ(rec.r, I2cResult::Ok);
    // Controller is free again.
    EXPECT_EQ(bus.write_async(0x68, payload, 1, rec_cb, &rec), I2cResult::Ok);
}

TEST_F(I2cIrqTest, Async_Read_Delivers_Bytes_Then_Callback) {
    ScriptedIrqTransport bus(i2c(), gpio(), irq());
    bus.script = {RXD, RXD, DONE};
    bus.rx     = {0x7E, 0x01};
    std::uint8_t buf[2] = {0, 0};
    CbRec rec;

    EXPECT_EQ(bus.read_async(0x68, buf, sizeof(buf), rec_cb, &rec), I2cResult::Ok);
    bus.handle_irq();
    EXPECT_EQ(rec.n, 1);
    EXPECT_EQ(rec.r, I2cResult::Ok);
    EXPECT_EQ(buf[0], 0x7EU);
    EXPECT_EQ(buf[1], 0x01U);
}

#endif /*__i2c_irq_test_cpp__*/
