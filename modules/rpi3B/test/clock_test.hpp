#ifndef __clock_test_hpp__
#define __clock_test_hpp__

#include <gtest/gtest.h>
#include <sstream>
#include <vector>
#include <algorithm>
#include <unordered_map>

#include "clock.hpp"

class CLOCKTest : public ::testing::Test
{
    public:
        /**
         * @brief Memory region for CLOCK instance will be taken from m_memory_region,
         *        the CLOCK instance layout will be done from m_memory_region.
        */
        CLOCKTest() : m_memory_region(RPi3B::ClockRegistersAddress::Register::CM_GPn_MAX), 
                      m_clock(CLOCK(m_memory_region.data())) {
            /*
            ::printf("start memory_region: 0x%X ", m_memory_region.data());
            ::printf("end memory_region: 0x%X ", m_memory_region.data() + GPIORegistersAddress::Register::BCM2837_MAX+1);
            ::printf("GPIO: 0x%X ", &m_gpio.memory().m_register[0]);
            */
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