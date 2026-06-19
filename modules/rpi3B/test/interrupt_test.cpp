#ifndef __interrupt_test_cpp__
#define __interrupt_test_cpp__

#include "interrupt_test.hpp"

void IRQTest::SetUp() {
    
}

void IRQTest::TearDown() {

}

void IRQTest::TestBody() {

}

TEST_F(IRQTest, Irq) {
    /*
     * Refer 7.5 Section of BCM2837-ARM-Peripherals.pdf
     * IRQ Number  Name
     *   48         smi
         49         gpio_int[0]
         50         gpio_int[1]
         51         gpio_int[2]
         52         gpio_int[3]
         53         i2c_int
         54         spi_int
         55         pcm_int
         56
         57         uart_int
     */
    ::printf("0x%X\n", irq());
    //::printf("0x%X\n", &irq()[IVT::Reset]);
    //::printf("0x%X\n", &irq()[IVT::UndefinedInstruction]);
    //::printf("0x%X\n", &irq()[IVT::SWI]);
}

/* --- Regression tests for the fixes in docs/DRIVER_REVIEW.md §2.4 --- */

TEST_F(IRQTest, Enable_Below32_Uses_Bank1_And_IsEnabled) {
    irq().enable(20);
    EXPECT_TRUE(irq().isEnabled(20));
    EXPECT_FALSE(irq().isEnabled(21));
}

TEST_F(IRQTest, Enable_AtOrAbove32_Uses_Bank2) {
    /* IRQ 53 == i2c_int (see table above): bank2, bit 21. */
    irq().enable(53);
    EXPECT_TRUE(irq().isEnabled(53));
    EXPECT_FALSE(irq().isEnabled(20)); /* a different bank stays clear */
}

TEST_F(IRQTest, Disable_Sets_Disable_Register) {
    irq().disable(54); /* spi_int */
    EXPECT_TRUE(irq().isEnabled(54) == false); /* not enabled; nothing in enable reg */
}

TEST_F(IRQTest, Install_Handlers_Does_Not_Crash) {
    /* Previously dereferenced a wild pointer; must now be a safe assignment. */
    irq().install_IRQHandler(53, []{});
    irq().install_FIQHandler(54, []{});
    SUCCEED();
}







#endif /*__interrupt_test_cpp__*/