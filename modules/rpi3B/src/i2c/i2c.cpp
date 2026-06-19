#ifndef __i2c_cpp__
#define __i2c_cpp__

#include "i2c.hpp"

namespace {
    struct field { std::uint32_t shift; std::uint32_t width; };

    /// @brief Bit position/width of each Control (C) register field.
    field control_field(RPi3B::BSCRegistersAddress::Control f) {
        switch (f) {
            case RPi3B::BSCRegistersAddress::Control::READ:  return {0,  1};
            case RPi3B::BSCRegistersAddress::Control::CLEAR: return {4,  2};
            case RPi3B::BSCRegistersAddress::Control::ST:    return {7,  1};
            case RPi3B::BSCRegistersAddress::Control::INTD:  return {8,  1};
            case RPi3B::BSCRegistersAddress::Control::INTT:  return {9,  1};
            case RPi3B::BSCRegistersAddress::Control::INTR:  return {10, 1};
            case RPi3B::BSCRegistersAddress::Control::I2CEN: return {15, 1};
            default:                                         return {0, 32};
        }
    }

    /// @brief Bit position of each Status (S) register field (all width 1).
    std::uint32_t status_shift(RPi3B::BSCRegistersAddress::Status f) {
        switch (f) {
            case RPi3B::BSCRegistersAddress::Status::TA:     return 0;
            case RPi3B::BSCRegistersAddress::Status::DONE:   return 1;
            case RPi3B::BSCRegistersAddress::Status::TXW:    return 2;
            case RPi3B::BSCRegistersAddress::Status::RXR:    return 3;
            case RPi3B::BSCRegistersAddress::Status::TXD:    return 4;
            case RPi3B::BSCRegistersAddress::Status::RXD:    return 5;
            case RPi3B::BSCRegistersAddress::Status::TXE:    return 6;
            case RPi3B::BSCRegistersAddress::Status::RXF:    return 7;
            case RPi3B::BSCRegistersAddress::Status::ERR:    return 8;
            case RPi3B::BSCRegistersAddress::Status::CLKT_TO:return 9;
            default:                                         return 0;
        }
    }

    /// @brief Low `w` bits set, e.g. mask(2) == 0b11.
    std::uint32_t mask(std::uint32_t w) { return (w >= 32) ? ~0U : ~((~0U) << w); }
}

void I2C::set_control(RPi3B::BSCRegistersAddress::Control f, value_type value) {
    auto fld = control_field(f);
    /* clear the field first, then OR in the (masked) value -- read/modify/write */
    memory().m_register[RPi3B::BSCRegistersAddress::Register::C] &= ~(mask(fld.width) << fld.shift);
    memory().m_register[RPi3B::BSCRegistersAddress::Register::C] |= ((value & mask(fld.width)) << fld.shift);
}

void I2C::clr_control(RPi3B::BSCRegistersAddress::Control f) {
    auto fld = control_field(f);
    memory().m_register[RPi3B::BSCRegistersAddress::Register::C] &= ~(mask(fld.width) << fld.shift);
}

I2C::value_type I2C::get_control(RPi3B::BSCRegistersAddress::Control f) const {
    auto fld = control_field(f);
    return (memory().m_register[RPi3B::BSCRegistersAddress::Register::C] >> fld.shift) & mask(fld.width);
}

void I2C::clr_status(RPi3B::BSCRegistersAddress::Status f) {
    /* DONE/ERR/CLKT are write-1-to-clear; harmless for the read-only status bits. */
    memory().m_register[RPi3B::BSCRegistersAddress::Register::S] = (1U << status_shift(f));
}

I2C::value_type I2C::get_status(RPi3B::BSCRegistersAddress::Status f) const {
    return (memory().m_register[RPi3B::BSCRegistersAddress::Register::S] >> status_shift(f)) & 1U;
}

void I2C::enable() {
    set_control(RPi3B::BSCRegistersAddress::Control::I2CEN, 1);
}

void I2C::disable() {
    clr_control(RPi3B::BSCRegistersAddress::Control::I2CEN);
}

void I2C::slave_address(value_type addr) {
    /* A register holds a 7-bit slave address. */
    memory().m_register[RPi3B::BSCRegistersAddress::Register::A] = (addr & 0x7FU);
}

I2C::value_type I2C::slave_address() const {
    return memory().m_register[RPi3B::BSCRegistersAddress::Register::A] & 0x7FU;
}

void I2C::data_length(value_type len) {
    /* DLEN is a 16-bit byte count. */
    memory().m_register[RPi3B::BSCRegistersAddress::Register::DLEN] = (len & 0xFFFFU);
}

I2C::value_type I2C::data_length() const {
    return memory().m_register[RPi3B::BSCRegistersAddress::Register::DLEN] & 0xFFFFU;
}

void I2C::clock_divider(value_type divider) {
    /* DIV is a 16-bit divider of the nominal 150MHz core clock. */
    memory().m_register[RPi3B::BSCRegistersAddress::Register::DIV] = (divider & 0xFFFFU);
}

I2C::value_type I2C::clock_divider() const {
    return memory().m_register[RPi3B::BSCRegistersAddress::Register::DIV] & 0xFFFFU;
}

void I2C::clear_fifo() {
    set_control(RPi3B::BSCRegistersAddress::Control::CLEAR, 0b01);
}

void I2C::start_read() {
    enable();
    set_control(RPi3B::BSCRegistersAddress::Control::READ, 1);
    set_control(RPi3B::BSCRegistersAddress::Control::ST, 1);
}

void I2C::start_write() {
    enable();
    set_control(RPi3B::BSCRegistersAddress::Control::READ, 0);
    set_control(RPi3B::BSCRegistersAddress::Control::ST, 1);
}

void I2C::write_byte(value_type byte) {
    memory().m_register[RPi3B::BSCRegistersAddress::Register::FIFO] = (byte & 0xFFU);
}

I2C::value_type I2C::read_byte() const {
    return memory().m_register[RPi3B::BSCRegistersAddress::Register::FIFO] & 0xFFU;
}

#endif /*__i2c_cpp__*/
