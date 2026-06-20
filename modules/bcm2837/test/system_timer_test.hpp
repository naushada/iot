#ifndef __system_timer_test_hpp__
#define __system_timer_test_hpp__

#include <gtest/gtest.h>
#include <cstdint>
#include <vector>

#include "system_timer.hpp"

/**
 * @brief SystemTimer over a std::vector-backed register block — the same
 *        placement-new seam the other bcm2837 drivers use for host tests.
 */
class SystemTimerTest : public ::testing::Test {
    public:
        using Reg = BCM2837::SystemTimerRegistersAddress::Register;

        SystemTimerTest()
            : m_reg(Reg::ST_MAX), m_timer(m_reg.data()) {}

        void poke(Reg r, std::uint32_t v) { m_reg[r] = v; }
        std::uint32_t peek(Reg r) const   { return m_reg[r]; }

    protected:
        std::vector<std::uint32_t> m_reg;
        SystemTimer m_timer;
};

#endif /*__system_timer_test_hpp__*/
