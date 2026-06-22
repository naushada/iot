#ifndef __spi_test_hpp__
#define __spi_test_hpp__

#include <gtest/gtest.h>
#include <vector>
#include <cstring>

#include "spi.hpp"

class SPITest : public ::testing::Test
{
    public:
        /**
         * @brief Memory region for the SPI instance is taken from
         *        m_memory_region so register access hits the buffer, not MMIO.
        */
        SPITest() : m_memory_region(BCM2837::SPIRegistersAddress::Register::SPI_MAX),
                    m_spi(SPI(m_memory_region.data())) {
            // Zero the overlaid registers. The register block has a trivial ctor
            // (so it doesn't clobber live MMIO), which lets -O2/-O3 drop the
            // vector's value-init as a dead store; zeroing after the overlay is
            // observed by the driver's volatile reads and is not elided.
            std::memset(m_memory_region.data(), 0,
                        m_memory_region.size() * sizeof(std::uint32_t));
        }
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
