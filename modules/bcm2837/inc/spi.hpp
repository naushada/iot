#ifndef __spi_hpp__
#define __spi_hpp__

#include <cstdint>
#include <cstdio>
#include <atomic>

#include "memory_map.hpp"

/**
 * @brief
 *      SPI0 master driver for the BCM2837. Follows the same framework as
 *      GPIO/CLOCK/I2C: a reference to a placement-new register block (real MMIO
 *      on hardware, a test buffer in unit tests) with field-level set/clr/get
 *      accessors over the CS register plus a few transaction helpers.
 *
 *      CS fields are addressed directly by datasheet bit position (see
 *      docs/DRIVER_REVIEW.md).
*/
class SPI {
    public:
        using value_type = std::uint32_t;

        SPI() : m_memory(*new BCM2837::SPIRegistersAddress) {}
        template<typename Region>
        SPI(Region region) : m_memory(*new(region) BCM2837::SPIRegistersAddress) {}
        ~SPI() = default;

        /* ---- CS (control/status) field access ---- */
        void set_cs(BCM2837::SPIRegistersAddress::ControlStatus field, value_type value);
        void clr_cs(BCM2837::SPIRegistersAddress::ControlStatus field);
        value_type get_cs(BCM2837::SPIRegistersAddress::ControlStatus field) const;

        /* ---- High level helpers ---- */
        /// @brief Select clock polarity/phase (SPI mode 0..3).
        void mode(value_type cpol, value_type cpha);
        /// @brief Select the active chip-select line (0..2).
        void chip_select(value_type ce);
        value_type chip_select() const;
        /// @brief CLK divider: SCLK = core_clock / divider (divider is even).
        void clock_divider(value_type divider);
        value_type clock_divider() const;
        /// @brief Clear the TX and RX FIFOs (CS.CLEAR = 0b11).
        void clear_fifo();
        /// @brief Begin/end a transfer (CS.TA).
        void begin_transfer();
        void end_transfer();
        /// @brief Push one byte to / pop one byte from the FIFO.
        void write_byte(value_type byte);
        value_type read_byte() const;
        /// @brief Status helpers (read-only CS bits).
        bool is_done() const;
        bool can_write() const;
        bool can_read() const;

        BCM2837::SPIRegistersAddress& memory() const {
            return(m_memory);
        }

    private:
        BCM2837::SPIRegistersAddress& m_memory;
};

#endif /*__spi_hpp__*/
