#ifndef __gpio_cpp__
#define __gpio_cpp__

#include "gpio.hpp"

/**
 * @brief
 *      All GPIO registers (GPFSELn, GPSETn, GPCLRn, GPLEVn, GPEDSn, ...) are
 *      organised by *BCM GPIO number*, NOT by 40-pin-header physical pin number.
 *      GPIO 0..9 live in bank/GPFSEL0, 10..19 in GPFSEL1, and so on. The earlier
 *      gpioNo2PinNo indirection mapped the GPIO number to a header pin and then
 *      indexed the registers by that pin, which addressed the wrong pin on real
 *      hardware (see docs/DRIVER_REVIEW.md §2.2). The driver now indexes the
 *      registers directly by GPIO number.
*/
namespace {
    /// @brief BCM2837 exposes GPIO 0..53.
    constexpr GPIO::gpio_number GPIO_MAX = 53;
    inline bool valid(GPIO::gpio_number g) { return g <= GPIO_MAX; }
}

void GPIO::write32(gpio_number gpio_n, std::uint32_t value) {
    if(valid(gpio_n)) {
        memory().m_register[gpio_n / 10] = value;
    }
}

void GPIO::write(gpio_number gpio_n, BCM2837::GPIORegistersAddress::Config cfg) {
    /**
     * @brief First clear the 3-bit FSEL field, then OR in the requested config.
    */
    if(valid(gpio_n)) {
        memory().m_register[(gpio_n / 10)] &= (~(07U << ((gpio_n % 10) * 3U)));
        memory().m_register[(gpio_n / 10)] |= (cfg << ((gpio_n % 10) * 3U));
    }
}

std::uint32_t GPIO::read32(gpio_number gpio_n) {
    if(valid(gpio_n)) {
        return(memory().m_register[gpio_n / 10]);
    }
    return(0);
}

std::uint32_t GPIO::read(gpio_number gpio_n) {
    if(valid(gpio_n)) {
        return((memory().m_register[gpio_n / 10] >> ((gpio_n % 10U) * 3U)) & 0b111);
    }
    return(0);
}

std::uint32_t GPIO::GPLEVn(gpio_number gpio_n) const {
    if(valid(gpio_n)) {
        if(gpio_n < 32) {
            return((memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPLEV0] >> gpio_n) & 1U);
        }
        return((memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPLEV1] >> (gpio_n - 32)) & 1U);
    }
    return(0);
}

void GPIO::GPLEVn(gpio_number gpio_n) {
    if(valid(gpio_n)) {
        if(gpio_n < 32) {
            memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPLEV0] |= (1U << gpio_n);
        } else {
            memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPLEV1] |= (1U << (gpio_n - 32));
        }
    }
}

void GPIO::output(gpio_number gpio_n) {
    /* @brief
     *  Each 32-bit GPFSELn holds ten 3-bit fields. Clear the field first so a
     *  pin previously set to an alternate function becomes a clean output
     *  (0b001), instead of OR-ing bit0 into a stale value.
     */
    if(valid(gpio_n)) {
        memory().m_register[(gpio_n / 10)] &= (~(7U << ((gpio_n % 10) * 3U)));
        memory().m_register[(gpio_n / 10)] |= (1U << ((gpio_n % 10) * 3U));
    }
}

void GPIO::input(gpio_number gpio_n) {
    /* @brief Input mode is 0b000: simply clear the 3-bit FSEL field. */
    if(valid(gpio_n)) {
        memory().m_register[(gpio_n / 10)] &= (~(7U << ((gpio_n % 10U) * 3U)));
    }
}

void GPIO::GPCLRn(gpio_number gpio_n) {
    if(valid(gpio_n)) {
        if(gpio_n < 32) {
            memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPCLR0] |= (1U << gpio_n);
        } else {
            memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPCLR1] |= (1U << (gpio_n - 32));
        }
    }
}

void GPIO::GPSETn(gpio_number gpio_n) {
    if(valid(gpio_n)) {
        if(gpio_n < 32) {
            memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPSET0] |= (1U << gpio_n);
        } else {
            memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPSET1] |= (1U << (gpio_n - 32));
        }
    }
}

std::uint32_t GPIO::GPGETn(gpio_number gpio_n) {
    if(valid(gpio_n)) {
        if(gpio_n < 32) {
            return((memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPSET0] >> gpio_n) & 01U);
        }
        return((memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPSET1] >> (gpio_n - 32)) & 01U);
    }
    return(0);
}

void GPIO::GPEDSn(gpio_number gpio_n) {
    if(valid(gpio_n)) {
        if(gpio_n < 32) {
            memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPEDS0] |= (1U << gpio_n);
        } else {
            memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPEDS1] |= (1U << (gpio_n - 32));
        }
    }
}

std::uint32_t GPIO::GPEDSn(gpio_number gpio_n) const {
    if(valid(gpio_n)) {
        if(gpio_n < 32) {
            return((memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPEDS0] >> gpio_n) & 01U);
        }
        return((memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPEDS1] >> (gpio_n - 32)) & 01U);
    }
    return(0);
}

void GPIO::GPRENn(gpio_number gpio_n) {
    if(valid(gpio_n)) {
        if(gpio_n < 32) {
            memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPREN0] |= (1U << gpio_n);
        } else {
            memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPREN1] |= (1U << (gpio_n - 32));
        }
    }
}

std::uint32_t GPIO::GPRENn(gpio_number gpio_n) const {
    if(valid(gpio_n)) {
        if(gpio_n < 32) {
            return((memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPREN0] >> gpio_n) & 01U);
        }
        return((memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPREN1] >> (gpio_n - 32)) & 01U);
    }
    return(0);
}

void GPIO::GPFENn(gpio_number gpio_n) {
    if(valid(gpio_n)) {
        if(gpio_n < 32) {
            memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPFEN0] |= (1U << gpio_n);
        } else {
            memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPFEN1] |= (1U << (gpio_n - 32));
        }
    }
}

std::uint32_t GPIO::GPFENn(gpio_number gpio_n) const {
    if(valid(gpio_n)) {
        if(gpio_n < 32) {
            return((memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPFEN0] >> gpio_n) & 01U);
        }
        return((memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPFEN1] >> (gpio_n - 32)) & 01U);
    }
    return(0);
}

void GPIO::GPHENn(gpio_number gpio_n) {
    if(valid(gpio_n)) {
        if(gpio_n < 32) {
            memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPHEN0] |= (1U << gpio_n);
        } else {
            memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPHEN1] |= (1U << (gpio_n - 32));
        }
    }
}

std::uint32_t GPIO::GPHENn(gpio_number gpio_n) const {
    if(valid(gpio_n)) {
        if(gpio_n < 32) {
            return((memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPHEN0] >> gpio_n) & 01U);
        }
        return((memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPHEN1] >> (gpio_n - 32)) & 01U);
    }
    return(0);
}

void GPIO::GPLENn(gpio_number gpio_n) {
    if(valid(gpio_n)) {
        if(gpio_n < 32) {
            memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPLEN0] |= (1U << gpio_n);
        } else {
            memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPLEN1] |= (1U << (gpio_n - 32));
        }
    }
}

std::uint32_t GPIO::GPLENn(gpio_number gpio_n) const {
    if(valid(gpio_n)) {
        if(gpio_n < 32) {
            return((memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPLEN0] >> gpio_n) & 01U);
        }
        return((memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPLEN1] >> (gpio_n - 32)) & 01U);
    }
    return(0);
}

void GPIO::GPARENn(gpio_number gpio_n) {
    if(valid(gpio_n)) {
        if(gpio_n < 32) {
            memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPAREN0] |= (1U << gpio_n);
        } else {
            memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPAREN1] |= (1U << (gpio_n - 32));
        }
    }
}

std::uint32_t GPIO::GPARENn(gpio_number gpio_n) const {
    if(valid(gpio_n)) {
        if(gpio_n < 32) {
            return((memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPAREN0] >> gpio_n) & 01U);
        }
        return((memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPAREN1] >> (gpio_n - 32)) & 01U);
    }
    return(0);
}

void GPIO::GPAFENn(gpio_number gpio_n) {
    if(valid(gpio_n)) {
        if(gpio_n < 32) {
            memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPAFEN0] |= (1U << gpio_n);
        } else {
            memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPAFEN1] |= (1U << (gpio_n - 32));
        }
    }
}

std::uint32_t GPIO::GPAFENn(gpio_number gpio_n) const {
    if(valid(gpio_n)) {
        if(gpio_n < 32) {
            return((memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPAFEN0] >> gpio_n) & 01U);
        }
        return((memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPAFEN1] >> (gpio_n - 32)) & 01U);
    }
    return(0);
}

void GPIO::GPPUD(gpio_number gpio_n, BCM2837::GPIORegistersAddress::PullUpDownConfig cfg) {
    /* GPPUD is a single global control (2-bit field); gpio_n is not an index. */
    (void)gpio_n;
    memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPPUD] &= (~3U);
    memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPPUD] |= (3U & cfg);
}

std::uint32_t GPIO::GPPUD(gpio_number gpio_n) const {
    (void)gpio_n;
    return(memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPPUD] & 3U);
}

void GPIO::GPPUDCLKn(gpio_number gpio_n) {
    if(valid(gpio_n)) {
        if(gpio_n < 32) {
            memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPPUDCLK0] |= (1U << gpio_n);
        } else {
            memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPPUDCLK1] |= (1U << (gpio_n - 32));
        }
    }
}

std::uint32_t GPIO::GPPUDCLKn(gpio_number gpio_n) const {
    if(valid(gpio_n)) {
        if(gpio_n < 32) {
            return((memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPPUDCLK0] >> gpio_n) & 01U);
        }
        return((memory().m_register[BCM2837::GPIORegistersAddress::Register::BCM2837_GPPUDCLK1] >> (gpio_n - 32)) & 01U);
    }
    return(0);
}

#endif /*__gpio_cpp__*/
