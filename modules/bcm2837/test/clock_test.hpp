#ifndef __clock_test_hpp__
#define __clock_test_hpp__

#include <gtest/gtest.h>
#include <sstream>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <cstring>

#include "clock.hpp"

class CLOCKTest : public ::testing::Test
{
    public:
        /**
         * @brief Memory region for CLOCK instance will be taken from m_memory_region,
         *        the CLOCK instance layout will be done from m_memory_region.
        */
        CLOCKTest() : m_memory_region(BCM2837::ClockRegistersAddress::Register::CM_GPn_MAX),
                      m_clock(CLOCK(m_memory_region.data())) {
            // Reset the overlaid registers to a known 0 state. The driver
            // placement-new's a register block whose constructor is trivial (so
            // it does not clobber live MMIO), which ends the vector elements'
            // lifetimes; at -O2/-O3 the compiler then drops the vector's value
            // initialisation as a dead store, leaving the registers as heap
            // garbage. Zeroing *after* the overlay is observed by the driver's
            // volatile reads, so it is not elided.
            std::memset(m_memory_region.data(), 0,
                        m_memory_region.size() * sizeof(std::uint32_t));
        }
        virtual ~CLOCKTest() override = default;
        
        virtual void SetUp() override;
        virtual void TearDown() override;
        virtual void TestBody() override;

        CLOCK clock() const {
            return(m_clock);
        }

    private:
        std::vector<std::uint32_t> m_memory_region;
        CLOCK m_clock;
};

#endif /*__clock_test_hpp__*/