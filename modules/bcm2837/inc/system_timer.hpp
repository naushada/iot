#ifndef __system_timer_hpp__
#define __system_timer_hpp__

#include <cstdint>

#include "memory_map.hpp"

/**
 * @file system_timer.hpp
 * @brief BCM2837 System Timer driver — a free-running 1 MHz microsecond clock
 *        plus four compare channels.
 *
 * Same framework as the other drivers (GPIO/CLOCK/I2C): a reference to a
 * placement-new'd register block — live MMIO on hardware (BCM2837::map_systimer())
 * or a std::vector-backed buffer in unit tests — with field-level accessors.
 *
 * It exists to give the interrupt-driven I²C transport a real time source for
 * its watchdog (see docs/i2c-irq-transport-spec.md §4.7): `now_us()` bounds the
 * wait by wall-clock microseconds instead of a unitless spin count, and `arm()`
 * lets a bare-metal build schedule a compare IRQ so a wedged bus still wakes the
 * core from `WFI`.
 *
 * NOTE: on the Raspberry Pi, compare channels 0 and 2 are owned by the GPU
 * firmware — only channels 1 and 3 are free for the ARM. `arm()` defaults to
 * channel 1.
 */
class SystemTimer {
    public:
        using value_type = std::uint32_t;

        /// Channels 1 and 3 are ARM-usable (0/2 belong to the GPU).
        static constexpr unsigned kArmChannel = 1U;

        SystemTimer() : m_memory(*new BCM2837::SystemTimerRegistersAddress) {}

        template<typename Region>
        SystemTimer(Region region)
            : m_memory(*new(region) BCM2837::SystemTimerRegistersAddress) {}

        ~SystemTimer() = default;

        /// @brief 64-bit free-running microsecond counter (CHI:CLO), read
        ///        glitch-free across a low-half rollover. 1 tick == 1 µs.
        std::uint64_t now_us() const;

        /// @brief Low 32 bits of the counter (CLO). Wraps every ~71.6 min, but
        ///        unsigned deltas over one wrap stay correct — enough for the
        ///        millisecond-scale watchdog deadlines.
        value_type now_lo() const;

        /// @brief Arm compare channel `ch` to match when CLO reaches
        ///        `deadline_lo`. The caller enables the System-Timer IRQ for
        ///        that channel separately (this only programs Cn). `ch` is
        ///        clamped to 0..3; prefer 1 or 3 on the Pi.
        void arm(value_type deadline_lo, unsigned ch = kArmChannel);

        /// @brief True if compare channel `ch` has matched (CS bit Mch set).
        bool matched(unsigned ch = kArmChannel) const;

        /// @brief Clear the match flag for channel `ch` (write-1-to-clear) so it
        ///        can re-arm.
        void clear_match(unsigned ch = kArmChannel);

        BCM2837::SystemTimerRegistersAddress& memory() const {
            return(m_memory);
        }

    private:
        BCM2837::SystemTimerRegistersAddress& m_memory;
};

#endif /*__system_timer_hpp__*/
