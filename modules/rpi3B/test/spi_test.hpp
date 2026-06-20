#ifndef __spi_test_hpp__
#define __spi_test_hpp__

#include <gtest/gtest.h>
#include <vector>

#include "spi.hpp"

class SPITest : public ::testing::Test
{
    public:
        /**
         * @brief Memory region for the SPI instance is taken from
         *        m_memory_region so register access hits the buffer, not MMIO.
        */
        SPITest() : m_memory_region(RPi3B::SPIRegistersAddress::Register::SPI_MAX),
                    m_spi(SPI(m_memory_region.data())) {}
        virtual ~SPITest() override = default;

        virtual void SetUp() override;
        virtual void TearDown() override;
        virtual void TestBody() override;

        SPI spi() const {
            return(m_spi);
        }

    private:
        std::vector<std::uint32_t> m_memory_region;
        SPI m_spi;
};

#endif /*__spi_test_hpp__*/
