#ifndef __i2c_test_hpp__
#define __i2c_test_hpp__

#include <gtest/gtest.h>
#include <vector>

#include "i2c.hpp"

class I2CTest : public ::testing::Test
{
    public:
        /**
         * @brief Memory region for the I2C instance is taken from
         *        m_memory_region so register access hits the buffer, not MMIO.
        */
        I2CTest() : m_memory_region(BCM2837::BSCRegistersAddress::Register::BSC_MAX),
                    m_i2c(I2C(m_memory_region.data())) {}
        virtual ~I2CTest() override = default;

        virtual void SetUp() override;
        virtual void TearDown() override;
        virtual void TestBody() override;

        I2C i2c() const {
            return(m_i2c);
        }

    private:
        std::vector<std::uint32_t> m_memory_region;
        I2C m_i2c;
};

#endif /*__i2c_test_hpp__*/
