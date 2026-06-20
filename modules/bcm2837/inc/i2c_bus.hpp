#ifndef __i2c_bus_hpp__
#define __i2c_bus_hpp__

#include <cstdint>
#include <cstddef>

#include "i2c.hpp"
#include "gpio.hpp"

/**
 * @file i2c_bus.hpp
 * @brief I²C transaction layer for the BCM2837 BSC1 controller.
 *
 * The `I2C` driver (i2c.hpp) is a field-level register model: it can set the
 * slave address, DLEN, the FIFO and the control/status bits, but it does not
 * sequence a real transfer. This layer adds that sequencing — and, just as
 * importantly, an abstract `I2cTransport` seam that sensor drivers depend on so
 * they are host-unit-testable against a fake.
 *
 *   I2cTransport            -- pure-virtual byte-stream transport (mockable)
 *   Bcm2837I2cTransport     -- drives BSC1: ALT0 pin-mux + clock divider +
 *                              FIFO pump against the DONE/ERR/CLKT status bits
 *
 * On hardware, construct over live MMIO (BCM2837::map_i2c() / map_gpiomem());
 * in tests, construct over std::vector-backed I2C/GPIO blocks — the same
 * placement-new seam the rest of the driver uses.
 */

/// @brief Outcome of an I²C transfer. Negative == failure.
enum class I2cResult : int {
    Ok           =  0,  ///< Transfer completed (S.DONE, no error).
    BadArg       = -1,  ///< Null buffer / zero length / address out of range.
    Timeout      = -2,  ///< S.DONE never asserted within the spin budget.
    Nack         = -3,  ///< Slave did not ACK (S.ERR).
    ClockTimeout = -4,  ///< Slave stretched the clock past the limit (S.CLKT).
};

/**
 * @brief Abstract I²C master transport — the seam sensor drivers depend on.
 *
 * Implementations move raw bytes to/from a 7-bit slave. `write_read` is the
 * register-read primitive every sensor needs (push a register pointer, then
 * read N bytes back).
 */
class I2cTransport {
    public:
        virtual ~I2cTransport() = default;

        /// @brief Write `len` bytes to `addr`.
        virtual I2cResult write(std::uint8_t addr,
                                const std::uint8_t* buf, std::size_t len) = 0;

        /// @brief Read `len` bytes from `addr`.
        virtual I2cResult read(std::uint8_t addr,
                               std::uint8_t* buf, std::size_t len) = 0;

        /// @brief Write `wlen` bytes, then read `rlen` bytes from `addr`.
        virtual I2cResult write_read(std::uint8_t addr,
                                     const std::uint8_t* wbuf, std::size_t wlen,
                                     std::uint8_t* rbuf, std::size_t rlen) = 0;

        /// @brief Read `n` bytes starting at 8-bit register pointer `reg`.
        I2cResult read_reg(std::uint8_t addr, std::uint8_t reg,
                           std::uint8_t* buf, std::size_t n) {
            return write_read(addr, &reg, 1, buf, n);
        }

        /// @brief Write one byte `value` to 8-bit register `reg`.
        I2cResult write_reg(std::uint8_t addr, std::uint8_t reg,
                            std::uint8_t value) {
            const std::uint8_t b[2] = { reg, value };
            return write(addr, b, sizeof(b));
        }
};

/**
 * @brief BSC1-backed transport for the Raspberry Pi 3B.
 *
 * Holds an `I2C` and a `GPIO` handle by value (each is a cheap wrapper over a
 * register-block reference). `bus_init()` muxes GPIO2/GPIO3 to ALT0 (SDA1/SCL1),
 * programs the clock divider and enables the controller; the transfer methods
 * drive the FIFO and poll the status register.
 *
 * BSC1 has no native repeated-START, so `write_read` issues the write and the
 * read as two back-to-back transactions (each with its own STOP). The mangOH
 * sensors (BMI160/BME680/light) tolerate this register-read style.
 */
class Bcm2837I2cTransport : public I2cTransport {
    public:
        /// @brief ~100 kHz from a 250 MHz core clock (tune on hardware).
        static constexpr std::uint16_t kDefaultDivider = 2500;
        /// @brief Upper bound on status-poll iterations before Timeout.
        static constexpr std::uint32_t kDefaultSpins   = 1000000U;

        Bcm2837I2cTransport(I2C i2c, GPIO gpio,
                            std::uint16_t divider   = kDefaultDivider,
                            std::uint32_t max_spins = kDefaultSpins)
            : m_i2c(i2c), m_gpio(gpio),
              m_divider(divider), m_max_spins(max_spins) {}

        /// @brief Mux GPIO2/3 → ALT0, set the clock divider, enable BSC1.
        void bus_init();

        I2cResult write(std::uint8_t addr,
                        const std::uint8_t* buf, std::size_t len) override;
        I2cResult read(std::uint8_t addr,
                       std::uint8_t* buf, std::size_t len) override;
        I2cResult write_read(std::uint8_t addr,
                             const std::uint8_t* wbuf, std::size_t wlen,
                             std::uint8_t* rbuf, std::size_t rlen) override;

        I2C&  i2c()  { return m_i2c; }
        GPIO& gpio() { return m_gpio; }

    protected:
        /// @brief Snapshot of the live Status (S) register. Virtual so a host
        ///        test can simulate a slave (assert DONE/RXD/TXD, feed the FIFO)
        ///        without real MMIO. Production reads the register verbatim.
        virtual std::uint32_t poll_status();

    private:
        /// @brief Clear FIFO + DONE/ERR/CLKT and program A/DLEN for a transfer.
        void prime(std::uint8_t addr, std::size_t len);
        /// @brief Map a terminal status word to a result (Ok/Nack/ClockTimeout).
        static I2cResult classify(std::uint32_t status);

        I2C           m_i2c;
        GPIO          m_gpio;
        std::uint16_t m_divider;
        std::uint32_t m_max_spins;
};

#endif /*__i2c_bus_hpp__*/
