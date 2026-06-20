#ifndef __bme680_cpp__
#define __bme680_cpp__

#include "bme680.hpp"

namespace {
    /* BME680 register map (datasheet §5.2). */
    constexpr std::uint8_t REG_CHIP_ID  = 0xD0;
    constexpr std::uint8_t REG_CTRL_HUM = 0x72;
    constexpr std::uint8_t REG_CTRL_MEAS= 0x74;
    constexpr std::uint8_t REG_FIELD0   = 0x1F;  /* press MSB; 8 bytes P/T/H */

    /* Oversampling: humidity x1, temperature x2, pressure x16, forced mode. */
    constexpr std::uint8_t OSRS_H_X1     = 0x01;
    constexpr std::uint8_t CTRL_MEAS_RUN = (0x02 << 5) | (0x05 << 2) | 0x01;

    std::uint16_t u16(std::uint8_t lsb, std::uint8_t msb) {
        return static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(lsb) | (static_cast<std::uint16_t>(msb) << 8));
    }
}

bool Bme680::probe() {
    std::uint8_t id = 0;
    return read_u8(REG_CHIP_ID, id) && id == kChipId;
}

bool Bme680::read_calibration() {
    std::uint8_t e9=0,ea=0, t2l=0,t2m=0,t3=0;
    std::uint8_t p1l=0,p1m=0,p2l=0,p2m=0,p3=0,p4l=0,p4m=0,p5l=0,p5m=0,p6=0,p7=0,
                 p8l=0,p8m=0,p9l=0,p9m=0,p10=0;
    std::uint8_t e1=0,e2=0,e3=0,e4=0,e5=0,e6=0,e7=0,e8=0;

    bool ok = true;
    /* Temperature coeffs. */
    ok = ok && read_u8(0xE9, e9)  && read_u8(0xEA, ea);
    ok = ok && read_u8(0x8A, t2l) && read_u8(0x8B, t2m) && read_u8(0x8C, t3);
    /* Pressure coeffs. */
    ok = ok && read_u8(0x8E, p1l) && read_u8(0x8F, p1m);
    ok = ok && read_u8(0x90, p2l) && read_u8(0x91, p2m) && read_u8(0x92, p3);
    ok = ok && read_u8(0x94, p4l) && read_u8(0x95, p4m);
    ok = ok && read_u8(0x96, p5l) && read_u8(0x97, p5m);
    ok = ok && read_u8(0x99, p6)  && read_u8(0x98, p7);
    ok = ok && read_u8(0x9C, p8l) && read_u8(0x9D, p8m);
    ok = ok && read_u8(0x9E, p9l) && read_u8(0x9F, p9m) && read_u8(0xA0, p10);
    /* Humidity coeffs (packed nibbles for H1/H2). */
    ok = ok && read_u8(0xE1, e1) && read_u8(0xE2, e2) && read_u8(0xE3, e3);
    ok = ok && read_u8(0xE4, e4) && read_u8(0xE5, e5) && read_u8(0xE6, e6);
    ok = ok && read_u8(0xE7, e7) && read_u8(0xE8, e8);
    if (!ok) {
        return false;
    }

    Calib& c = m_calib;
    c.t1 = u16(e9, ea);
    c.t2 = static_cast<std::int16_t>(u16(t2l, t2m));
    c.t3 = static_cast<std::int8_t>(t3);

    c.p1 = u16(p1l, p1m);
    c.p2 = static_cast<std::int16_t>(u16(p2l, p2m));
    c.p3 = static_cast<std::int8_t>(p3);
    c.p4 = static_cast<std::int16_t>(u16(p4l, p4m));
    c.p5 = static_cast<std::int16_t>(u16(p5l, p5m));
    c.p6 = static_cast<std::int8_t>(p6);
    c.p7 = static_cast<std::int8_t>(p7);
    c.p8 = static_cast<std::int16_t>(u16(p8l, p8m));
    c.p9 = static_cast<std::int16_t>(u16(p9l, p9m));
    c.p10 = p10;

    c.h1 = static_cast<std::uint16_t>((static_cast<std::uint16_t>(e3) << 4) | (e2 & 0x0F));
    c.h2 = static_cast<std::uint16_t>((static_cast<std::uint16_t>(e1) << 4) | (e2 >> 4));
    c.h3 = static_cast<std::int8_t>(e4);
    c.h4 = static_cast<std::int8_t>(e5);
    c.h5 = static_cast<std::int8_t>(e6);
    c.h6 = e7;
    c.h7 = static_cast<std::int8_t>(e8);
    return true;
}

bool Bme680::init() {
    return read_calibration();
}

std::int32_t Bme680::compensate_temperature(std::int32_t temp_adc, Calib& c) {
    std::int32_t var1 = (temp_adc >> 3) - (static_cast<std::int32_t>(c.t1) << 1);
    std::int32_t var2 = (var1 * static_cast<std::int32_t>(c.t2)) >> 11;
    std::int32_t var3 = ((((var1 >> 1) * (var1 >> 1)) >> 12)
                         * (static_cast<std::int32_t>(c.t3) << 4)) >> 14;
    c.t_fine = var2 + var3;
    return ((c.t_fine * 5) + 128) >> 8;   /* 0.01 °C */
}

std::int32_t Bme680::compensate_pressure(std::int32_t press_adc, const Calib& c) {
    std::int32_t var1 = (c.t_fine >> 1) - 64000;
    std::int32_t var2 = ((((var1 >> 2) * (var1 >> 2)) >> 11)
                         * static_cast<std::int32_t>(c.p6)) >> 2;
    var2 = var2 + ((var1 * static_cast<std::int32_t>(c.p5)) << 1);
    var2 = (var2 >> 2) + (static_cast<std::int32_t>(c.p4) << 16);
    var1 = (((((var1 >> 2) * (var1 >> 2)) >> 13)
             * (static_cast<std::int32_t>(c.p3) << 5)) >> 3)
           + ((static_cast<std::int32_t>(c.p2) * var1) >> 1);
    var1 = var1 >> 18;
    var1 = ((32768 + var1) * static_cast<std::int32_t>(c.p1)) >> 15;
    if (var1 == 0) {
        return 0;   /* avoid divide-by-zero on bad calibration */
    }
    std::int32_t press = 1048576 - press_adc;
    press = (press - (var2 >> 12)) * 3125;
    if (press >= 0x40000000) {
        press = (press / var1) << 1;
    } else {
        press = (press << 1) / var1;
    }
    var1 = (static_cast<std::int32_t>(c.p9)
            * (((press >> 3) * (press >> 3)) >> 13)) >> 12;
    var2 = ((press >> 2) * static_cast<std::int32_t>(c.p8)) >> 13;
    std::int32_t var3 = ((press >> 8) * (press >> 8) * (press >> 8)
                         * static_cast<std::int32_t>(c.p10)) >> 17;
    press = press + ((var1 + var2 + var3
                      + (static_cast<std::int32_t>(c.p7) << 7)) >> 4);
    return press;   /* Pa */
}

std::int32_t Bme680::compensate_humidity(std::int32_t hum_adc, const Calib& c) {
    const std::int32_t temp_scaled = ((c.t_fine * 5) + 128) >> 8;
    std::int32_t var1 = hum_adc - (static_cast<std::int32_t>(c.h1) << 4)
                        - (((temp_scaled * static_cast<std::int32_t>(c.h3)) / 100) >> 1);
    std::int32_t var2 = (static_cast<std::int32_t>(c.h2)
        * (((temp_scaled * static_cast<std::int32_t>(c.h4)) / 100)
           + (((temp_scaled * ((temp_scaled * static_cast<std::int32_t>(c.h5)) / 100)) >> 6) / 100)
           + (1 << 14))) >> 10;
    std::int32_t var3 = var1 * var2;
    std::int32_t var4 = static_cast<std::int32_t>(c.h6) << 7;
    var4 = (var4 + ((temp_scaled * static_cast<std::int32_t>(c.h7)) / 100)) >> 4;
    std::int32_t var5 = ((var3 >> 14) * (var3 >> 14)) >> 10;
    std::int32_t var6 = (var4 * var5) >> 1;
    std::int32_t hum = (((var3 + var6) >> 10) * 1000) >> 12;   /* milli-%RH */
    if (hum > 100000) {
        hum = 100000;
    } else if (hum < 0) {
        hum = 0;
    }
    return hum;
}

bool Bme680::read(Sample& out) {
    /* Humidity oversampling must be written before ctrl_meas latches the mode. */
    if (write_u8(REG_CTRL_HUM, OSRS_H_X1) != I2cResult::Ok) {
        return false;
    }
    if (write_u8(REG_CTRL_MEAS, CTRL_MEAS_RUN) != I2cResult::Ok) {
        return false;
    }
    /* On hardware the sampler waits for the measurement to finish (status
       0x1D bit7) before reading; field0 is read here in one 8-byte burst. */
    std::uint8_t d[8] = {0};
    if (read_regs(REG_FIELD0, d, sizeof(d)) != I2cResult::Ok) {
        return false;
    }
    const std::int32_t press_adc =
        (static_cast<std::int32_t>(d[0]) << 12) | (static_cast<std::int32_t>(d[1]) << 4) | (d[2] >> 4);
    const std::int32_t temp_adc =
        (static_cast<std::int32_t>(d[3]) << 12) | (static_cast<std::int32_t>(d[4]) << 4) | (d[5] >> 4);
    const std::int32_t hum_adc =
        (static_cast<std::int32_t>(d[6]) << 8) | d[7];

    const std::int32_t t = compensate_temperature(temp_adc, m_calib);
    const std::int32_t p = compensate_pressure(press_adc, m_calib);
    const std::int32_t h = compensate_humidity(hum_adc, m_calib);

    out.temperature_c = static_cast<double>(t) / 100.0;
    out.pressure_pa   = static_cast<double>(p);
    out.humidity_pct  = static_cast<double>(h) / 1000.0;
    return true;
}

#endif /*__bme680_cpp__*/
