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

        CLOCK() : m_memory(*new RPi3B::ClockRegistersAddress) {}

        template<typename Region>
        CLOCK(Region region) : m_memory(*new(region) RPi3B::ClockRegistersAddress) {}

        ~CLOCK() = default;

        void set_CM_GPnDIV(RPi3B::ClockRegistersAddress::Register reg, RPi3B::ClockRegistersAddress::ClockDivisor field, divisor_type value);
        void clr_CM_GPnDIV(RPi3B::ClockRegistersAddress::Register reg, RPi3B::ClockRegistersAddress::ClockDivisor field);
        divisor_type get_CM_GPnDIV(RPi3B::ClockRegistersAddress::Register reg, RPi3B::ClockRegistersAddress::ClockDivisor field);

        void set_CM_GPnCTL(RPi3B::ClockRegistersAddress::Register reg, RPi3B::ClockRegistersAddress::CM_GPnCTL_Type field, divisor_type value);
        void clr_CM_GPnCTL(RPi3B::ClockRegistersAddress::Register reg, RPi3B::ClockRegistersAddress::CM_GPnCTL_Type field);
        control_type get_CM_GPnCTL(RPi3B::ClockRegistersAddress::Register reg, RPi3B::ClockRegistersAddress::CM_GPnCTL_Type field);


        void CM_GP0DIV(RPi3B::ClockRegistersAddress::ClockDivisor divisor, divisor_type value);
        divisor_type CM_GP0DIV(RPi3B::ClockRegistersAddress::ClockDivisor divisor) const;
        void CM_GP1DIV(RPi3B::ClockRegistersAddress::ClockDivisor divisor, divisor_type value);
        divisor_type CM_GP1DIV(RPi3B::ClockRegistersAddress::ClockDivisor divisor) const;
        void CM_GP2DIV(RPi3B::ClockRegistersAddress::ClockDivisor divisor, divisor_type value);
        divisor_type CM_GP2DIV(RPi3B::ClockRegistersAddress::ClockDivisor divisor) const;
        void write(RPi3B::ClockRegistersAddress::Register reg, RPi3B::ClockRegistersAddress::ClockDivisor divif ,divisor_type value);
        divisor_type read(RPi3B::ClockRegistersAddress::Register, RPi3B::ClockRegistersAddress::ClockDivisor divif) const;

        /**
         * @brief
         *      CLOCK Control Interface
        */
        void CM_GP0CTL(RPi3B::ClockRegistersAddress::CM_GPnCTL_Type ctl, divisor_type value);
        divisor_type CM_GP0CTL(RPi3B::ClockRegistersAddress::CM_GPnCTL_Type) const;
        void CM_GP1CTL(RPi3B::ClockRegistersAddress::CM_GPnCTL_Type ctl, divisor_type value);
        divisor_type CM_GP1CTL(RPi3B::ClockRegistersAddress::CM_GPnCTL_Type) const;
        void CM_GP2CTL(RPi3B::ClockRegistersAddress::CM_GPnCTL_Type ctl, divisor_type value);
        divisor_type CM_GP2CTL(RPi3B::ClockRegistersAddress::CM_GPnCTL_Type) const;
        void write(RPi3B::ClockRegistersAddress::Register reg, RPi3B::ClockRegistersAddress::CM_GPnCTL_Type type ,divisor_type value);
        divisor_type read(RPi3B::ClockRegistersAddress::Register, RPi3B::ClockRegistersAddress::CM_GPnCTL_Type type) const;

        auto& memory() const {
            return(m_memory);
        }

    private:
        RPi3B::ClockRegistersAddress& m_memory;
};







#endif /*__clock_hpp__*/