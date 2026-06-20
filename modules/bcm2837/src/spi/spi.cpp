#ifndef __spi_cpp__
#define __spi_cpp__

#include "spi.hpp"

namespace {
    struct field { std::uint32_t shift; std::uint32_t width; };

    /// @brief Bit position/width of each CS register field.
    field cs_field(BCM2837::SPIRegistersAddress::ControlStatus f) {
        using CS = BCM2837::SPIRegistersAddress::ControlStatus;
        switch (f) {
            case CS::CS_LINE:  return {0,  2};
            case CS::CPHA:     return {2,  1};
            case CS::CPOL:     return {3,  1};
            case CS::CLEAR:    return {4,  2};
            case CS::CSPOL:    return {6,  1};
            case CS::TA:       return {7,  1};
            case CS::DMAEN:    return {8,  1};
            case CS::INTD:     return {9,  1};
            case CS::INTR:     return {10, 1};
            case CS::ADCS:     return {11, 1};
            case CS::REN:      return {12, 1};
            case CS::LEN:      return {13, 1};
            case CS::LMONO:    return {14, 1};
            case CS::TE_EN:    return {15, 1};
            case CS::DONE:     return {16, 1};
            case CS::RXD:      return {17, 1};
            case CS::TXD:      return {18, 1};
            case CS::RXR:      return {19, 1};
            case CS::RXF:      return {20, 1};
            case CS::CSPOL0:   return {21, 1};
            case CS::CSPOL1:   return {22, 1};
            case CS::CSPOL2:   return {23, 1};
            case CS::DMA_LEN:  return {24, 1};
            case CS::LEN_LONG: return {25, 1};
            default:           return {0, 32};
        }
    }

    std::uint32_t mask(std::uint32_t w) { return (w >= 32) ? ~0U : ~((~0U) << w); }
}

void SPI::set_cs(BCM2837::SPIRegistersAddress::ControlStatus f, value_type value) {
    auto fld = cs_field(f);
    /* clear the field first, then OR in the (masked) value -- read/modify/write */
    memory().m_register[BCM2837::SPIRegistersAddress::Register::CS] &= ~(mask(fld.width) << fld.shift);
    memory().m_register[BCM2837::SPIRegistersAddress::Register::CS] |= ((value & mask(fld.width)) << fld.shift);
}

void SPI::clr_cs(BCM2837::SPIRegistersAddress::ControlStatus f) {
    auto fld = cs_field(f);
    memory().m_register[BCM2837::SPIRegistersAddress::Register::CS] &= ~(mask(fld.width) << fld.shift);
}

SPI::value_type SPI::get_cs(BCM2837::SPIRegistersAddress::ControlStatus f) const {
    auto fld = cs_field(f);
    return (memory().m_register[BCM2837::SPIRegistersAddress::Register::CS] >> fld.shift) & mask(fld.width);
}

void SPI::mode(value_type cpol, value_type cpha) {
    set_cs(BCM2837::SPIRegistersAddress::ControlStatus::CPOL, cpol);
    set_cs(BCM2837::SPIRegistersAddress::ControlStatus::CPHA, cpha);
}

void SPI::chip_select(value_type ce) {
    set_cs(BCM2837::SPIRegistersAddress::ControlStatus::CS_LINE, ce);
}

SPI::value_type SPI::chip_select() const {
    return get_cs(BCM2837::SPIRegistersAddress::ControlStatus::CS_LINE);
}

void SPI::clock_divider(value_type divider) {
    /* CLK is a 16-bit divider; SCLK = core_clock / divider. */
    memory().m_register[BCM2837::SPIRegistersAddress::Register::CLK] = (divider & 0xFFFFU);
}

SPI::value_type SPI::clock_divider() const {
    return memory().m_register[BCM2837::SPIRegistersAddress::Register::CLK] & 0xFFFFU;
}

void SPI::clear_fifo() {
    set_cs(BCM2837::SPIRegistersAddress::ControlStatus::CLEAR, 0b11);
}

void SPI::begin_transfer() {
    set_cs(BCM2837::SPIRegistersAddress::ControlStatus::TA, 1);
}

void SPI::end_transfer() {
    clr_cs(BCM2837::SPIRegistersAddress::ControlStatus::TA);
}

void SPI::write_byte(value_type byte) {
    memory().m_register[BCM2837::SPIRegistersAddress::Register::FIFO] = (byte & 0xFFU);
}

SPI::value_type SPI::read_byte() const {
    return memory().m_register[BCM2837::SPIRegistersAddress::Register::FIFO] & 0xFFU;
}

bool SPI::is_done() const {
    return get_cs(BCM2837::SPIRegistersAddress::ControlStatus::DONE) == 1U;
}

bool SPI::can_write() const {
    return get_cs(BCM2837::SPIRegistersAddress::ControlStatus::TXD) == 1U;
}

bool SPI::can_read() const {
    return get_cs(BCM2837::SPIRegistersAddress::ControlStatus::RXD) == 1U;
}

#endif /*__spi_cpp__*/
