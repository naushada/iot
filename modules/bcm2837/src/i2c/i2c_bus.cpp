#ifndef __i2c_bus_cpp__
#define __i2c_bus_cpp__

#include "i2c_bus.hpp"

namespace {
    /* BSC Status (S) register bit masks — see memory_map.hpp / i2c.cpp. */
    constexpr std::uint32_t S_DONE = 1U << 1;  ///< Transfer Done
    constexpr std::uint32_t S_TXD  = 1U << 4;  ///< FIFO can accept data
    constexpr std::uint32_t S_RXD  = 1U << 5;  ///< FIFO contains data
    constexpr std::uint32_t S_ERR  = 1U << 8;  ///< Slave NACK (ACK error)
    constexpr std::uint32_t S_CLKT = 1U << 9;  ///< Clock stretch timeout

    constexpr std::uint8_t  ADDR_MAX = 0x7F;   ///< 7-bit address space
}

void Bcm2837I2cTransport::bus_init() {
    /* GPIO2 = SDA1, GPIO3 = SCL1; both take ALT0 to reach the BSC1 block. */
    m_gpio.write(2, BCM2837::GPIORegistersAddress::Config::AlternateFunction0);
    m_gpio.write(3, BCM2837::GPIORegistersAddress::Config::AlternateFunction0);
    m_i2c.clock_divider(m_divider);
    m_i2c.enable();
}

std::uint32_t Bcm2837I2cTransport::poll_status() {
    /* Verbatim read of the live Status register. Tests override this to emulate
       a slave without real MMIO. */
    return m_i2c.memory().m_register[BCM2837::BSCRegistersAddress::Register::S];
}

void Bcm2837I2cTransport::prime(std::uint8_t addr, std::size_t len) {
    m_i2c.clear_fifo();
    m_i2c.clr_status(BCM2837::BSCRegistersAddress::Status::DONE);
    m_i2c.clr_status(BCM2837::BSCRegistersAddress::Status::ERR);
    m_i2c.clr_status(BCM2837::BSCRegistersAddress::Status::CLKT_TO);
    m_i2c.slave_address(addr);
    m_i2c.data_length(static_cast<I2C::value_type>(len));
}

I2cResult Bcm2837I2cTransport::classify(std::uint32_t status) {
    if (status & S_ERR)  return I2cResult::Nack;
    if (status & S_CLKT) return I2cResult::ClockTimeout;
    return I2cResult::Ok;
}

I2cResult Bcm2837I2cTransport::write(std::uint8_t addr,
                                     const std::uint8_t* buf, std::size_t len) {
    if (buf == nullptr || len == 0 || addr > ADDR_MAX) {
        return I2cResult::BadArg;
    }
    prime(addr, len);

    std::size_t i = 0;
    m_i2c.start_write();
    for (std::uint32_t spin = 0; spin < m_max_spins; ++spin) {
        const std::uint32_t s = poll_status();
        if (s & S_ERR)  return I2cResult::Nack;
        if (s & S_CLKT) return I2cResult::ClockTimeout;
        /* Feed the FIFO while there is room and bytes remain. */
        if ((s & S_TXD) && i < len) {
            m_i2c.write_byte(buf[i++]);
            continue;
        }
        if (s & S_DONE) {
            return classify(s);
        }
    }
    return I2cResult::Timeout;
}

I2cResult Bcm2837I2cTransport::read(std::uint8_t addr,
                                    std::uint8_t* buf, std::size_t len) {
    if (buf == nullptr || len == 0 || addr > ADDR_MAX) {
        return I2cResult::BadArg;
    }
    prime(addr, len);

    std::size_t i = 0;
    m_i2c.start_read();
    for (std::uint32_t spin = 0; spin < m_max_spins; ++spin) {
        const std::uint32_t s = poll_status();
        if (s & S_ERR)  return I2cResult::Nack;
        if (s & S_CLKT) return I2cResult::ClockTimeout;
        /* Drain the FIFO before honouring DONE so trailing bytes are not lost. */
        if ((s & S_RXD) && i < len) {
            buf[i++] = static_cast<std::uint8_t>(m_i2c.read_byte() & 0xFFU);
            continue;
        }
        if (s & S_DONE) {
            /* DONE with bytes still owed and nothing left in the FIFO == short
               read; report it rather than returning garbage. */
            return (i < len) ? I2cResult::Timeout : classify(s);
        }
    }
    return I2cResult::Timeout;
}

I2cResult Bcm2837I2cTransport::write_read(std::uint8_t addr,
                                          const std::uint8_t* wbuf, std::size_t wlen,
                                          std::uint8_t* rbuf, std::size_t rlen) {
    /* BSC1 has no repeated-START: issue the register-pointer write and the read
       as two back-to-back transactions (each with its own STOP). */
    const I2cResult w = write(addr, wbuf, wlen);
    if (w != I2cResult::Ok) {
        return w;
    }
    return read(addr, rbuf, rlen);
}

#endif /*__i2c_bus_cpp__*/
