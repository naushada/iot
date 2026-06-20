#ifndef __fake_i2c_transport_hpp__
#define __fake_i2c_transport_hpp__

#include <cstdint>
#include <cstddef>

#include "i2c_bus.hpp"

/**
 * @brief In-memory I²C slave for host tests.
 *
 * Models a standard auto-incrementing register device: a 256-byte register
 * file plus a pointer set by the last write. The sensor drivers only ever talk
 * through read_reg/write_reg/write, so this is enough to exercise them without
 * any BSC1 hardware. Tests pre-load `regs[]` with the bytes a real chip would
 * return, then assert on the decoded result.
 */
class FakeI2cTransport : public I2cTransport {
    public:
        std::uint8_t  regs[256] = {};
        std::uint8_t  lastAddr  = 0;
        int           ptr       = 0;
        bool          nack      = false;   ///< force every transfer to NACK

        I2cResult write(std::uint8_t addr, const std::uint8_t* buf, std::size_t len) override {
            if (nack)        return I2cResult::Nack;
            if (!buf || !len) return I2cResult::BadArg;
            lastAddr = addr;
            ptr = buf[0];
            for (std::size_t i = 1; i < len; ++i) {
                regs[(ptr + static_cast<int>(i) - 1) & 0xFF] = buf[i];
            }
            return I2cResult::Ok;
        }

        I2cResult read(std::uint8_t addr, std::uint8_t* buf, std::size_t len) override {
            if (nack)        return I2cResult::Nack;
            if (!buf || !len) return I2cResult::BadArg;
            lastAddr = addr;
            for (std::size_t i = 0; i < len; ++i) {
                buf[i] = regs[(ptr + static_cast<int>(i)) & 0xFF];
            }
            ptr = (ptr + static_cast<int>(len)) & 0xFF;
            return I2cResult::Ok;
        }

        I2cResult write_read(std::uint8_t addr,
                             const std::uint8_t* wbuf, std::size_t wlen,
                             std::uint8_t* rbuf, std::size_t rlen) override {
            const I2cResult w = write(addr, wbuf, wlen);
            if (w != I2cResult::Ok) return w;
            return read(addr, rbuf, rlen);
        }
};

#endif /*__fake_i2c_transport_hpp__*/
