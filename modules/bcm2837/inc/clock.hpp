#ifndef __clock_hpp__
#define __clock_hpp__

#include <cstdint>
#include <cstdio>
#include <atomic>
#include "memory_map.hpp"


class CLOCK {
    public:
        using gpio_number = std::uint32_t;
        using divisor_type = std::uint32_t;
        using control_type = std::uint32_t;

        CLOCK() : m_memory(*new BCM2837::ClockRegistersAddress) {}

        template<typename Region>
        CLOCK(Region region) : m_memory(*new(region) BCM2837::ClockRegistersAddress) {}

        ~CLOCK() = default;

        void set_CM_GPnDIV(BCM2837::ClockRegistersAddress::Register reg, BCM2837::ClockRegistersAddress::ClockDivisor field, divisor_type value);
        void clr_CM_GPnDIV(BCM2837::ClockRegistersAddress::Register reg, BCM2837::ClockRegistersAddress::ClockDivisor field);
        divisor_type get_CM_GPnDIV(BCM2837::ClockRegistersAddress::Register reg, BCM2837::ClockRegistersAddress::ClockDivisor field);

        void set_CM_GPnCTL(BCM2837::ClockRegistersAddress::Register reg, BCM2837::ClockRegistersAddress::CM_GPnCTL_Type field, divisor_type value);
        void clr_CM_GPnCTL(BCM2837::ClockRegistersAddress::Register reg, BCM2837::ClockRegistersAddress::CM_GPnCTL_Type field);
        control_type get_CM_GPnCTL(BCM2837::ClockRegistersAddress::Register reg, BCM2837::ClockRegistersAddress::CM_GPnCTL_Type field);


        void CM_GP0DIV(BCM2837::ClockRegistersAddress::ClockDivisor divisor, divisor_type value);
        divisor_type CM_GP0DIV(BCM2837::ClockRegistersAddress::ClockDivisor divisor) const;
        void CM_GP1DIV(BCM2837::ClockRegistersAddress::ClockDivisor divisor, divisor_type value);
        divisor_type CM_GP1DIV(BCM2837::ClockRegistersAddress::ClockDivisor divisor) const;
        void CM_GP2DIV(BCM2837::ClockRegistersAddress::ClockDivisor divisor, divisor_type value);
        divisor_type CM_GP2DIV(BCM2837::ClockRegistersAddress::ClockDivisor divisor) const;
        void write(BCM2837::ClockRegistersAddress::Register reg, BCM2837::ClockRegistersAddress::ClockDivisor divif ,divisor_type value);
        divisor_type read(BCM2837::ClockRegistersAddress::Register, BCM2837::ClockRegistersAddress::ClockDivisor divif) const;

        /**
         * @brief
         *      CLOCK Control Interface
        */
        void CM_GP0CTL(BCM2837::ClockRegistersAddress::CM_GPnCTL_Type ctl, divisor_type value);
        divisor_type CM_GP0CTL(BCM2837::ClockRegistersAddress::CM_GPnCTL_Type) const;
        void CM_GP1CTL(BCM2837::ClockRegistersAddress::CM_GPnCTL_Type ctl, divisor_type value);
        divisor_type CM_GP1CTL(BCM2837::ClockRegistersAddress::CM_GPnCTL_Type) const;
        void CM_GP2CTL(BCM2837::ClockRegistersAddress::CM_GPnCTL_Type ctl, divisor_type value);
        divisor_type CM_GP2CTL(BCM2837::ClockRegistersAddress::CM_GPnCTL_Type) const;
        void write(BCM2837::ClockRegistersAddress::Register reg, BCM2837::ClockRegistersAddress::CM_GPnCTL_Type type ,divisor_type value);
        divisor_type read(BCM2837::ClockRegistersAddress::Register, BCM2837::ClockRegistersAddress::CM_GPnCTL_Type type) const;

        auto& memory() const {
            return(m_memory);
        }

    private:
        BCM2837::ClockRegistersAddress& m_memory;
};







#endif /*__clock_hpp__*/