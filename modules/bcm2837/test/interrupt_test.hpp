#ifndef __interrupt_test_hpp__
#define __interrupt_test_hpp__


#include <gtest/gtest.h>
#include <sstream>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <cstring>

#include "interrupt.hpp"

class IRQTest : public ::testing::Test
{
    public:
        /**
         * @brief Memory region for IRQ instance will be taken from m_memory_region,
         *        the IRQ instance layout will be done from m_memory_region.
        */
        IRQTest() : m_memory_region(BCM2837::InterruptRegisterAddress::Register::IRQs_ALL_MAX
                                    + (sizeof(IVT) + sizeof(std::uint32_t) - 1) / sizeof(std::uint32_t)),
                    m_irq(IRQ(m_memory_region.data())) {
            // The IRQ driver overlays the register block AND places an IVT right
            // past it (at region + IRQs_ALL_MAX), so the buffer must cover both —
            // otherwise install_*Handler writes past the end (the bus error).
            // Zero after the overlay: the register/IVT structs have trivial ctors
            // (so they don't clobber live MMIO), which lets -O2/-O3 drop the
            // vector's value-init as a dead store; zeroing here is observed by
            // the driver's volatile reads and is not elided.
            std::memset(m_memory_region.data(), 0,
                        m_memory_region.size() * sizeof(std::uint32_t));
        }

        virtual ~IRQTest() override = default;
        
        virtual void SetUp() override;
        virtual void TearDown() override;
        virtual void TestBody() override;

        IRQ irq() const {
            return(m_irq);
        }

    private:
        std::vector<std::uint32_t> m_memory_region;
        IRQ m_irq;
};









#endif /*__interrupt_test_hpp__*/