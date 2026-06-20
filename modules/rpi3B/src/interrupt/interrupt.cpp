#ifndef __interrupt_cpp__
#define __interrupt_cpp__

#include "interrupt.hpp"



/**
 * @brief
 *      IRQs 0..31 are controlled by the *_IRQs_1 register (bit = irq number);
 *      IRQs 32..63 by the *_IRQs_2 register (bit = irq number - 32). The earlier
 *      code had the register selection inverted and used out-of-range shifts
 *      (see docs/DRIVER_REVIEW.md §2.4).
*/
void IRQ::enable(irq_number irqNumber) {
    if(irqNumber < 32) {
        m_memory.m_register[RPi3B::InterruptRegisterAddress::Register::Enable_IRQs_1] |= (1U << irqNumber);
    } else {
        m_memory.m_register[RPi3B::InterruptRegisterAddress::Register::Enable_IRQs_2] |= (1U << (irqNumber - 32));
    }
}

void IRQ::disable(irq_number irqNumber) {
    if(irqNumber < 32) {
        m_memory.m_register[RPi3B::InterruptRegisterAddress::Register::Disable_IRQs_1] |= (1U << irqNumber);
    } else {
        m_memory.m_register[RPi3B::InterruptRegisterAddress::Register::Disable_IRQs_2] |= (1U << (irqNumber - 32));
    }
}

/**
 * @brief Report whether the interrupt was enabled, by reading back the bit we
 *        set in the Enable_IRQs register. This is side-effect free (the previous
 *        version mutated the read-only pending register during the query) and
 *        has the &/== precedence fixed.
*/
bool IRQ::isEnabled(irq_number number) {
    if(number < 32) {
        return(((m_memory.m_register[RPi3B::InterruptRegisterAddress::Register::Enable_IRQs_1] >> number) & 1U) == 1U);
    }
    return(((m_memory.m_register[RPi3B::InterruptRegisterAddress::Register::Enable_IRQs_2] >> (number - 32)) & 1U) == 1U);
}

/**
 * @brief Install the top-level IRQ/FIQ vector. The architectural IVT has a
 *        single IRQ (and FIQ) slot; per-peripheral dispatch happens downstream
 *        of that vector, so IRQNumber does not select a slot here. The previous
 *        version reinterpret_cast an (uninitialised) function-pointer slot to an
 *        IVT* and dereferenced it — a wild pointer.
*/
void IRQ::install_IRQHandler(std::uint8_t IRQNumber, IVT::pointerToFn cb) {
    (void)IRQNumber;
    m_ivt[IVT::Number::IRQ] = cb;
}

void IRQ::install_FIQHandler(std::uint8_t IRQNumber, IVT::pointerToFn cb) {
    (void)IRQNumber;
    m_ivt[IVT::Number::FIQ] = cb;
}








#endif /*__interrupt_cpp__*/