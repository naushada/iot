#ifndef __gpio_test_hpp__
#define __gpio_test_hpp__


#include <gtest/gtest.h>
#include <sstream>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <cstring>

#include "gpio.hpp"

class GPIOTest : public ::testing::Test
{
    public:
        /**
         * @brief Memory region for GPIO instance will be taken from m_memory_region,
         *        the GPIO instance layout will be done from m_memory_region.
        */
        GPIOTest() : m_memory_region(BCM2837::GPIORegistersAddress::Register::BCM2837_MAX), m_gpio(GPIO(m_memory_region.data())) {
            // Zero the overlaid registers. The register block has a trivial ctor
            // (so it doesn't clobber live MMIO), which lets -O2/-O3 drop the
            // vector's value-init as a dead store; zeroing after the overlay is
            // observed by the driver's volatile reads and is not elided.
            std::memset(m_memory_region.data(), 0,
                        m_memory_region.size() * sizeof(std::uint32_t));
        }
        virtual ~GPIOTest() override = default;
        
        virtual void SetUp() override;
        virtual void TearDown() override;
        virtual void TestBody() override;

        GPIO gpio() const {
            return(m_gpio);
        }

    private:
        std::vector<std::uint32_t> m_memory_region;
        GPIO m_gpio;
        std::unordered_map<std::string, std::string> m_gpio2PinMap;
};


#endif /*__gpio_test_hpp__*/