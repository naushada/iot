#ifndef __interrupt_hpp__
#define __interrupt_hpp__

#include <cstdint>
#include <cstdio>
#include <atomic>

#include "memory_map.hpp"

class IVT {
    public:
        using pointerToFn = void (*)();
        /// @brief Address of below will be 0x00, 0x04, 0x08, 0x0C, 0x10, 0x14, 0x18, 0x1C
        enum Number {
            Reset,
            UndefinedInstruction,
            SWI,
            InstructionAbort,
            DataAbort,
            Hypervisor,
            IRQ,
            FIQ,
            End
        };

        IVT() {}
        ~IVT() {}

        /** 
         * @brief Compiler give preference to this new operator over global new operator and invoke this new operator. 
         * @param nBytes Number of bytes to be allocated
         * @param region is the memory region to be return if present.
         * @return pointer to void
         */
        void *operator new(std::size_t nBytes, void *region=nullptr) {
            (void)nBytes;
            if(nullptr == region) {
                return reinterpret_cast<void *>((0x00000000U));
            }
            return(region);
        }

        pointerToFn &operator[](Number irq) {
            return(m_IRQTable[irq]);
        }

    private:
        pointerToFn m_IRQTable[End];
};

class IVT_Table {
    public:
        IVT_Table() : m_ivt(*new IVT) {}
        template<typename Region>
        IVT_Table(Region region) : m_ivt(*new(region) IVT) {}

    
    private:
        IVT& m_ivt;
};

class IRQ {
    public:
        using irq_number = std::uint8_t;

        //using IVT::IVT;

        IRQ() : m_memory(*new BCM2837::InterruptRegisterAddress), m_ivt(*new IVT) {}

        template<typename Region>
        IRQ(Region region) : m_memory(*new(region) BCM2837::InterruptRegisterAddress), m_ivt(*new (region + BCM2837::InterruptRegisterAddress::Register::IRQs_ALL_MAX) IVT) {}

        ~IRQ() = default;
        void enable(irq_number number);
        void disable(irq_number number);
        bool isEnabled(irq_number number);
        void install_IRQHandler(std::uint8_t IRQNumber, IVT::pointerToFn cb);
        void install_FIQHandler(std::uint8_t IRQNumber, IVT::pointerToFn cb);
        

    private:
        BCM2837::InterruptRegisterAddress& m_memory;
        IVT& m_ivt;
};








#endif /*__interrupt_hpp__*/