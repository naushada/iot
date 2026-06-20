#ifndef __system_timer_cpp__
#define __system_timer_cpp__

#include "system_timer.hpp"

namespace {
    using R = BCM2837::SystemTimerRegistersAddress::Register;

    /// Clamp a channel index to the four physical compare registers.
    inline unsigned clamp_ch(unsigned ch) { return ch > 3U ? 3U : ch; }
}

std::uint64_t SystemTimer::now_us() const {
    // The 64-bit counter is two 32-bit registers that tick independently, so a
    // naive {CHI, CLO} read can straddle a low-half rollover. Re-read CHI and
    // retry if it moved (datasheet §12) — at most one retry per ~71.6 minutes.
    for (;;) {
        const std::uint32_t hi  = m_memory.m_register[R::CHI];
        const std::uint32_t lo  = m_memory.m_register[R::CLO];
        const std::uint32_t hi2 = m_memory.m_register[R::CHI];
        if (hi == hi2) {
            return (static_cast<std::uint64_t>(hi) << 32) | lo;
        }
    }
}

SystemTimer::value_type SystemTimer::now_lo() const {
    return m_memory.m_register[R::CLO];
}

void SystemTimer::arm(value_type deadline_lo, unsigned ch) {
    m_memory.m_register[R::C0 + clamp_ch(ch)] = deadline_lo;
}

bool SystemTimer::matched(unsigned ch) const {
    return (m_memory.m_register[R::CS] & (1U << clamp_ch(ch))) != 0U;
}

void SystemTimer::clear_match(unsigned ch) {
    // CS match bits are write-1-to-clear.
    m_memory.m_register[R::CS] = (1U << clamp_ch(ch));
}

#endif /*__system_timer_cpp__*/
