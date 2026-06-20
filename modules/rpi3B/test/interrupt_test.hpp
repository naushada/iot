#ifndef __interrupt_test_hpp__
#define __interrupt_test_hpp__


#include <gtest/gtest.h>
#include <sstream>
#include <vector>
#include <algorithm>
#include <unordered_map>

#include "interrupt.hpp"

class IRQTest : public ::testing::Test
{
    public:
        /**
         * @brief Memory region for IRQ instance will be taken from m_memory_region,
         *        the IRQ instance layout will be done from m_memory_region.
        */
        IRQTest() : m_memory_region(RPi3B::InterruptRegisterAddress::Register::IRQs_ALL_MAX), 
                    m_irq(IRQ(m_memory_region.data())) {
            
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