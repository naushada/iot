#ifndef __i2c_dev_hpp__
#define __i2c_dev_hpp__

#include <cstdint>
#include <cstddef>
#include <cerrno>
#include <string>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#include "i2c_bus.hpp"   // I2cTransport / I2cResult

/**
 * @file i2c_dev.hpp
 * @brief Linux i2c-dev (/dev/i2c-N) implementation of I2cTransport.
 *
 * Drives the kernel I2C subsystem via `ioctl(I2C_RDWR)` instead of poking the
 * BCM2837 BSC registers directly. The practical wins over Bcm2837I2cTransport:
 *
 *   - **Unprivileged**: needs only r/w on `/dev/i2c-N` (group `i2c`), NOT
 *     `CAP_SYS_RAWIO` / `/dev/mem`. So the iot-sensord daemon can run as a
 *     normal service user — the privilege model the rest of the stack uses.
 *   - **Real repeated-START**: `write_read` is one combined transaction (the
 *     BSC controller has no native repeated-START, so the register transport
 *     issues two separate transfers).
 *   - The kernel `i2c_bcm2835` driver owns clocking / arbitration / pin-mux.
 *
 * Requires the device booted with i2c enabled (`dtparam=i2c_arm=on`) so
 * `/dev/i2c-1` exists. POSIX/Linux only (pulls in `<linux/i2c.h>`), like
 * mmio.hpp — header-only so the host driver/test build is unaffected.
 *
 * This is the PREFERRED transport on a booted Linux device; the BSC register
 * transport (Bcm2837I2cTransport) is the bare-metal / no-i2c-dev fallback.
 */
class I2cDevTransport : public I2cTransport {
    public:
        I2cDevTransport() = default;
        explicit I2cDevTransport(const std::string& dev) { open(dev); }
        ~I2cDevTransport() override { close(); }

        I2cDevTransport(const I2cDevTransport&) = delete;
        I2cDevTransport& operator=(const I2cDevTransport&) = delete;

        /// Open `/dev/i2c-N`. Returns 0 on success, -1 on failure (errno set).
        int open(const std::string& dev) {
            close();
            m_fd = ::open(dev.c_str(), O_RDWR | O_CLOEXEC);
            return m_fd < 0 ? -1 : 0;
        }
        void close() { if (m_fd >= 0) { ::close(m_fd); m_fd = -1; } }
        bool is_open() const { return m_fd >= 0; }

        I2cResult write(std::uint8_t addr,
                        const std::uint8_t* buf, std::size_t len) override {
            if (buf == nullptr || len == 0 || addr > kAddrMax) return I2cResult::BadArg;
            if (m_fd < 0) return I2cResult::Timeout;
            i2c_msg msg{};
            msg.addr  = addr;
            msg.flags = 0;                            // write
            msg.len   = static_cast<__u16>(len);
            msg.buf   = const_cast<std::uint8_t*>(buf);
            return xfer(&msg, 1);
        }

        I2cResult read(std::uint8_t addr,
                       std::uint8_t* buf, std::size_t len) override {
            if (buf == nullptr || len == 0 || addr > kAddrMax) return I2cResult::BadArg;
            if (m_fd < 0) return I2cResult::Timeout;
            i2c_msg msg{};
            msg.addr  = addr;
            msg.flags = I2C_M_RD;
            msg.len   = static_cast<__u16>(len);
            msg.buf   = buf;
            return xfer(&msg, 1);
        }

        I2cResult write_read(std::uint8_t addr,
                             const std::uint8_t* wbuf, std::size_t wlen,
                             std::uint8_t* rbuf, std::size_t rlen) override {
            if (wbuf == nullptr || rbuf == nullptr || wlen == 0 || rlen == 0 ||
                addr > kAddrMax) {
                return I2cResult::BadArg;
            }
            if (m_fd < 0) return I2cResult::Timeout;
            // Register-pointer write + read in ONE combined transaction — the
            // kernel issues a real repeated-START between the two messages.
            i2c_msg msgs[2]{};
            msgs[0].addr  = addr;
            msgs[0].flags = 0;
            msgs[0].len   = static_cast<__u16>(wlen);
            msgs[0].buf   = const_cast<std::uint8_t*>(wbuf);
            msgs[1].addr  = addr;
            msgs[1].flags = I2C_M_RD;
            msgs[1].len   = static_cast<__u16>(rlen);
            msgs[1].buf   = rbuf;
            return xfer(msgs, 2);
        }

    private:
        static constexpr std::uint8_t kAddrMax = 0x7F;   // 7-bit address space

        I2cResult xfer(i2c_msg* msgs, int n) {
            i2c_rdwr_ioctl_data io{};
            io.msgs  = msgs;
            io.nmsgs = static_cast<__u32>(n);
            if (::ioctl(m_fd, I2C_RDWR, &io) < 0) {
                // ENXIO / EREMOTEIO == slave didn't ACK → Nack; else Timeout.
                return (errno == ENXIO || errno == EREMOTEIO) ? I2cResult::Nack
                                                              : I2cResult::Timeout;
            }
            return I2cResult::Ok;
        }

        int m_fd = -1;
};

#endif /*__i2c_dev_hpp__*/
