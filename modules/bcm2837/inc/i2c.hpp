#ifndef __i2c_hpp__
#define __i2c_hpp__

#include <cstdint>
#include <cstdio>
#include <atomic>

#include "memory_map.hpp"

/**
 * @brief
 *      I2C master driver built on the BCM2837 Broadcom Serial Controller (BSC).
 *      Follows the same framework as GPIO/CLOCK: a reference to a placement-new
 *      register block (real MMIO on hardware, a test buffer in unit tests) and
 *      field-level set/clr/get accessors plus a few transaction helpers.
 *
 *      Register fields are addressed directly by datasheet bit position; there
 *      is no pin-number indirection (see docs/DRIVER_REVIEW.md).
*/
class I2C {
    public:
        using value_type = std::uint32_t;

        I2C() : m_memory(*new BCM2837::BSCRegistersAddress) {}
        template<typename Region>
        I2C(Region region) : m_memory(*new(region) BCM2837::BSCRegistersAddress) {}
        ~I2C() = default;

        /* ---- Control register (C) field access ---- */
        void set_control(BCM2837::BSCRegistersAddress::Control field, value_type value);
        void clr_control(BCM2837::BSCRegistersAddress::Control field);
        value_type get_control(BCM2837::BSCRegistersAddress::Control field) const;

        /* ---- Status register (S) field access (DONE/ERR/CLKT are W1C) ---- */
        void clr_status(BCM2837::BSCRegistersAddress::Status field);
        value_type get_status(BCM2837::BSCRegistersAddress::Status field) const;

        /* ---- High level helpers ---- */
        /// @brief Enable/disable the BSC controller (C.I2CEN).
        void enable();
        void disable();
        /// @brief Program the 7-bit slave address (A register).
        void slave_address(value_type addr);
        value_type slave_address() const;
        /// @brief Program the number of bytes for the next transfer (DLEN).
        void data_length(value_type len);
        value_type data_length() const;
        /// @brief Clock divider (DIV): SCL = core_clock / DIV.
        void clock_divider(value_type divider);
        value_type clock_divider() const;
        /// @brief Clear the FIFO (C.CLEAR).
        void clear_fifo();
        /// @brief Kick off a read or write transfer (sets READ/ST + I2CEN).
        void start_read();
        void start_write();
        /// @brief Push one byte into / pop one byte out of the FIFO.
        void write_byte(value_type byte);
        value_type read_byte() const;

        BCM2837::BSCRegistersAddress& memory() const {
            return(m_memory);
        }

    private:
        BCM2837::BSCRegistersAddress& m_memory;
};

#endif /*__i2c_hpp__*/
