#ifndef __clock_cpp__
#define __clock_cpp__

#include "clock.hpp"

void CLOCK::set_CM_GPnDIV(RPi3B::ClockRegistersAddress::Register reg, RPi3B::ClockRegistersAddress::ClockDivisor field, divisor_type value) {

    switch (field)
    {
    case RPi3B::ClockRegistersAddress::ClockDivisor::DIVI:
        {
            /// SET bits 12...23 ---- Integer Divisor
            memory().m_register[reg] |= (((~((~0U) << 12)) & value) << 12);
        }
        break;

        case RPi3B::ClockRegistersAddress::ClockDivisor::DIVF:
        {
            /// SET bits 0...11 --- Float divisor
            memory().m_register[reg] |= ((~((~0U) << 12)) & value);
        }
        break;

        case RPi3B::ClockRegistersAddress::ClockDivisor::BOTH_VALUE:
        {
            /// SET bits 0...23 --- both Integer & Float divisor
            memory().m_register[reg] |= ((~((~0U) << 24)) & value);
        }
        break;
    
        default:
            /// Error Handling
            break;
    }
}

void CLOCK::clr_CM_GPnDIV(RPi3B::ClockRegistersAddress::Register reg, RPi3B::ClockRegistersAddress::ClockDivisor field) {
    switch (field)
    {
    case RPi3B::ClockRegistersAddress::ClockDivisor::DIVI:
        {
            /// SET bits 12...23 ---- Integer Divisor
            memory().m_register[reg] &= ((~((~0U) << 24))  & (~((~0U) << 12)));
        }
        break;

        case RPi3B::ClockRegistersAddress::ClockDivisor::DIVF:
        {
            /// SET bits 0...11 --- Float divisor
            memory().m_register[reg] &= ((~0U) << 12);
        }
        break;

        case RPi3B::ClockRegistersAddress::ClockDivisor::BOTH_VALUE:
        {
            /// SET bits 0...23 --- both Integer & Float divisor
            memory().m_register[reg] &= ((~0U) << 24);
        }
        break;
    
        default:
            /// Error Handling
            break;
    }
}

CLOCK::divisor_type CLOCK::get_CM_GPnDIV(RPi3B::ClockRegistersAddress::Register reg, RPi3B::ClockRegistersAddress::ClockDivisor field) {

    CLOCK::divisor_type value = 0;
    switch (field)
    {
    case RPi3B::ClockRegistersAddress::ClockDivisor::DIVI:
        {
            /// SET bits 12...23 ---- Integer Divisor
            value = (memory().m_register[reg] >> 12) & (~((~0U) << 12));
        }
        break;

        case RPi3B::ClockRegistersAddress::ClockDivisor::DIVF:
        {
            /// SET bits 0...11 --- Float divisor
            value = memory().m_register[reg] & (~(1U << 12));
        }
        break;

        case RPi3B::ClockRegistersAddress::ClockDivisor::BOTH_VALUE:
        {
            /// SET bits 0...23 --- both Integer & Float divisor
            value = memory().m_register[reg] & (~(1U << 24));
        }
        break;
    
        default:
            /// Error Handling
            break;
    }

    return(value);
}

void CLOCK::set_CM_GPnCTL(RPi3B::ClockRegistersAddress::Register reg, RPi3B::ClockRegistersAddress::CM_GPnCTL_Type field, divisor_type value) {
    switch (field)
    {
    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::SRC:
        {
            memory().m_register[reg] |= (~(~0U << 4)) & value;
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::ENAB:
        {
            memory().m_register[reg] |= ((value & 01) << 4);
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::KILL:
        {
            memory().m_register[reg] |= ((value & 01) << 5);
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::BUSY:
        {
            /* Read only register */
        }
        break;
    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::FLIP:
        {
            /* Read only register */
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::MASH:
        {
            memory().m_register[reg] |= ((value & 0b11) << 8);
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::ALL:
        {
            memory().m_register[reg] = value;
        }
        break;

    
    default:
        break;
    }

}

void CLOCK::clr_CM_GPnCTL(RPi3B::ClockRegistersAddress::Register reg, RPi3B::ClockRegistersAddress::CM_GPnCTL_Type field) {
    switch (field)
    {
    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::SRC:
        {
            memory().m_register[reg] &= (~0b1111 << 0);
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::ENAB:
        {
            memory().m_register[reg] &= (~(1U << 4));
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::KILL:
        {
            memory().m_register[reg] &= (~(1U << 5));
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::BUSY:
        {
            /* Read only register */
        }
        break;
    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::FLIP:
        {
            /* Read only register */
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::MASH:
        {
            memory().m_register[reg] &= (~(0b11 << 8));
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::ALL:
        {
            memory().m_register[reg] &= (~(0b1111 | (0b1 << 4) | (0b1 << 5) | (01 << 6) | (0b1 << 7) | (0b11 << 8)));
        }
        break;

    
    default:
        break;
    }
}

CLOCK::control_type CLOCK::get_CM_GPnCTL(RPi3B::ClockRegistersAddress::Register reg, RPi3B::ClockRegistersAddress::CM_GPnCTL_Type field) {
    CLOCK::control_type value = 0;
    switch (field)
    {
    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::SRC:
        {
            value = memory().m_register[reg] & 0b1111;
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::ENAB:
        {
            value = (memory().m_register[reg] >> 4) & 01;
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::KILL:
        {
            value = (memory().m_register[reg] >> 5) & 01;
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::BUSY:
        {
            /* Read only register */
        }
        break;
    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::FLIP:
        {
            /* Read only register */
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::MASH:
        {
            value = (memory().m_register[reg] >> 8) & 0b11;
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::ALL:
        {
            value = memory().m_register[reg];
        }
        break;

    
    default:
        break;
    }

    return(value);
}


void CLOCK::CM_GP0DIV(RPi3B::ClockRegistersAddress::ClockDivisor divisor, CLOCK::divisor_type value) {

    switch (divisor)
    {
        case RPi3B::ClockRegistersAddress::ClockDivisor::DIVI:
        {
            /// SET bits 12...23 ---- Integer Divisor
            memory().m_register[RPi3B::ClockRegistersAddress::Register::CM_GP0DIV] |= (((~((~0U) << 12)) & value) << 12);
        }
        break;

        case RPi3B::ClockRegistersAddress::ClockDivisor::DIVF:
        {
            /// SET bits 0...11 --- Float divisor
            memory().m_register[RPi3B::ClockRegistersAddress::Register::CM_GP0DIV] |= ((~((~0U) << 12)) & value);
        }
        break;

        case RPi3B::ClockRegistersAddress::ClockDivisor::BOTH_VALUE:
        {
            /// SET bits 0...23 --- both Integer & Float divisor
            memory().m_register[RPi3B::ClockRegistersAddress::Register::CM_GP0DIV] |= ((~((~0U) << 24)) & value);
        }
        break;
    
        default:
            /// Error Handling
            break;
    }
}

CLOCK::divisor_type CLOCK::CM_GP0DIV(RPi3B::ClockRegistersAddress::ClockDivisor divisor) const {
    divisor_type value = 0;

    switch (divisor)
    {
        case RPi3B::ClockRegistersAddress::ClockDivisor::DIVI:
        {
            /// SET bits 12...23 ---- Integer Divisor
            value = (memory().m_register[RPi3B::ClockRegistersAddress::Register::CM_GP0DIV] >> 12) & (~((~0U) << 12));
        }
        break;

        case RPi3B::ClockRegistersAddress::ClockDivisor::DIVF:
        {
            /// SET bits 0...11 --- Float divisor
            value = memory().m_register[RPi3B::ClockRegistersAddress::Register::CM_GP0DIV] & (~((~0U) << 12U));
        }
        break;

        case RPi3B::ClockRegistersAddress::ClockDivisor::BOTH_VALUE:
        {
            /// SET bits 0...23 --- both Integer & Float divisor
            value = memory().m_register[RPi3B::ClockRegistersAddress::Register::CM_GP0DIV] & (~((~0U) << 24U));
        }
        break;
    
        default:
            /// Error Handling
            break;
    }
    return(value);
}

void CLOCK::CM_GP1DIV(RPi3B::ClockRegistersAddress::ClockDivisor divisor, CLOCK::divisor_type value) {
    switch (divisor)
    {
        case RPi3B::ClockRegistersAddress::ClockDivisor::DIVI:
        {
            /// SET bits 12...23 ---- Integer Divisor
            memory().m_register[RPi3B::ClockRegistersAddress::Register::CM_GP1DIV] |= (((~((~0U) << 12)) & value) << 12);
        }
        break;

        case RPi3B::ClockRegistersAddress::ClockDivisor::DIVF:
        {
            /// SET bits 0...11 --- Float divisor
            memory().m_register[RPi3B::ClockRegistersAddress::Register::CM_GP1DIV] |= (~((~0U) << 12) & value);
        }
        break;

        case RPi3B::ClockRegistersAddress::ClockDivisor::BOTH_VALUE:
        {
            /// SET bits 0...23 --- both Integer & Float divisor
            memory().m_register[RPi3B::ClockRegistersAddress::Register::CM_GP1DIV] |= (~((~0U) << 24) & value);
        }
        break;
    
        default:
            /// Error Handling
            break;
    }
}

CLOCK::divisor_type CLOCK::CM_GP1DIV(RPi3B::ClockRegistersAddress::ClockDivisor divisor) const {
    divisor_type value = 0;

    switch (divisor)
    {
        case RPi3B::ClockRegistersAddress::ClockDivisor::DIVI:
        {
            /// SET bits 12...23 ---- Integer Divisor
            value = (memory().m_register[RPi3B::ClockRegistersAddress::Register::CM_GP1DIV] >> 12) & (~((~0U) << 12));
        }
        break;

        case RPi3B::ClockRegistersAddress::ClockDivisor::DIVF:
        {
            /// SET bits 0...11 --- Float divisor
            value = memory().m_register[RPi3B::ClockRegistersAddress::Register::CM_GP1DIV] & (~((~0U) << 12));
        }
        break;

        case RPi3B::ClockRegistersAddress::ClockDivisor::BOTH_VALUE:
        {
            /// SET bits 0...23 --- both Integer & Float divisor
            value = memory().m_register[RPi3B::ClockRegistersAddress::Register::CM_GP1DIV] & (~((~0U) << 24));
        }
        break;
    
        default:
            /// Error Handling
            break;
    }
    return(value);
}

void CLOCK::CM_GP2DIV(RPi3B::ClockRegistersAddress::ClockDivisor divisor, CLOCK::divisor_type value) {
    switch (divisor)
    {
        case RPi3B::ClockRegistersAddress::ClockDivisor::DIVI:
        {
            /// SET bits 12...23 ---- Integer Divisor
            memory().m_register[RPi3B::ClockRegistersAddress::Register::CM_GP2DIV] |= ((~((~0U) << 12) & value) << 12);
        }
        break;

        case RPi3B::ClockRegistersAddress::ClockDivisor::DIVF:
        {
            /// SET bits 0...11 --- Float divisor
            memory().m_register[RPi3B::ClockRegistersAddress::Register::CM_GP2DIV] |= (~((~0U) << 12) & value);
        }
        break;

        case RPi3B::ClockRegistersAddress::ClockDivisor::BOTH_VALUE:
        {
            /// SET bits 0...23 --- both Integer & Float divisor
            memory().m_register[RPi3B::ClockRegistersAddress::Register::CM_GP2DIV] |= (~((~0U) << 24) & value);
        }
        break;
    
        default:
            /// Error Handling
            break;
    }
}

CLOCK::divisor_type CLOCK::CM_GP2DIV(RPi3B::ClockRegistersAddress::ClockDivisor divisor) const {
    divisor_type value = 0;

    switch (divisor)
    {
        case RPi3B::ClockRegistersAddress::ClockDivisor::DIVI:
        {
            /// SET bits 12...23 ---- Integer Divisor
            value = (memory().m_register[RPi3B::ClockRegistersAddress::Register::CM_GP2DIV] >> 12) & (~((~0U) << 12));
        }
        break;

        case RPi3B::ClockRegistersAddress::ClockDivisor::DIVF:
        {
            /// SET bits 0...11 --- Float divisor
            value = memory().m_register[RPi3B::ClockRegistersAddress::Register::CM_GP2DIV] & (~((~0U) << 12));
        }
        break;

        case RPi3B::ClockRegistersAddress::ClockDivisor::BOTH_VALUE:
        {
            /// SET bits 0...23 --- both Integer & Float divisor
            value = memory().m_register[RPi3B::ClockRegistersAddress::Register::CM_GP2DIV] & (~((~0U) << 24));
        }
        break;
    
        default:
            /// Error Handling
            break;
    }
    return(value);
}

void CLOCK::write(RPi3B::ClockRegistersAddress::Register reg, RPi3B::ClockRegistersAddress::ClockDivisor divif, CLOCK::divisor_type value) {

    switch (reg)
    {
        case RPi3B::ClockRegistersAddress::Register::CM_GP0DIV:
        {
            CM_GP0DIV(divif, value);
        }
        break;

        case RPi3B::ClockRegistersAddress::Register::CM_GP1DIV:
        {
            CM_GP1DIV(divif, value);
        }
        break;

        case RPi3B::ClockRegistersAddress::Register::CM_GP2DIV:
        {
            CM_GP2DIV(divif, value);
        }
        break;
    
        default:
            /// Error Handling
            break;
    }
}

CLOCK::divisor_type CLOCK::read(RPi3B::ClockRegistersAddress::Register reg, RPi3B::ClockRegistersAddress::ClockDivisor divif) const {

    CLOCK::divisor_type value = 0;
    switch (reg)
    {
        case RPi3B::ClockRegistersAddress::Register::CM_GP0DIV:
        {
            value = CM_GP0DIV(divif);
        }
        break;

        case RPi3B::ClockRegistersAddress::Register::CM_GP1DIV:
        {
            value = CM_GP1DIV(divif);
        }
        break;

        case RPi3B::ClockRegistersAddress::Register::CM_GP2DIV:
        {
            value = CM_GP2DIV(divif);
        }
        break;
    
        default:
            /// Error Handling
            break;
    }
    return(value);
}

void CLOCK::CM_GP0CTL(RPi3B::ClockRegistersAddress::CM_GPnCTL_Type ctl, divisor_type value) {
    switch (ctl)
    {
    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::SRC:
        {
            memory().m_register[RPi3B::ClockRegistersAddress::CM_GP0CTL] |= (~(~0U << 4)) & value;
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::ENAB:
        {
            memory().m_register[RPi3B::ClockRegistersAddress::CM_GP0CTL] |= ((value & 01) << 5);
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::KILL:
        {
            memory().m_register[RPi3B::ClockRegistersAddress::CM_GP0CTL] |= ((value & 01) << 6);
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::BUSY:
        {
            /* Read only register */
        }
        break;
    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::FLIP:
        {
            /* Read only register */
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::MASH:
        {
            memory().m_register[RPi3B::ClockRegistersAddress::CM_GP0CTL] |= ((value & (~(~0U << 2))) << 10);
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::ALL:
        {
            auto all_value = 0;
            all_value = (~(~0U << 4)) & value | //SRC
                        ((value & 01) << 5) | // ENAB
                        ((value & 01) << 6) | //KILL
                        ((value & (~(~0U << 2))) << 10); //MASH
            memory().m_register[RPi3B::ClockRegistersAddress::CM_GP0CTL] = all_value;

        }
        break;

    
    default:
        break;
    }

}

CLOCK::control_type CLOCK::CM_GP0CTL(RPi3B::ClockRegistersAddress::CM_GPnCTL_Type ctlType) const {
    
    CLOCK::control_type value = 0;
    switch (ctlType)
    {
    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::SRC:
        {
            value = memory().m_register[RPi3B::ClockRegistersAddress::CM_GP0CTL] & (~(~0U << 4));
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::ENAB:
        {
            value = (memory().m_register[RPi3B::ClockRegistersAddress::CM_GP0CTL] >> 5) & 01;
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::KILL:
        {
            value = (memory().m_register[RPi3B::ClockRegistersAddress::CM_GP0CTL] >> 6) & 01;
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::BUSY:
        {
            /* Read only register */
        }
        break;
    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::FLIP:
        {
            /* Read only register */
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::MASH:
        {
            value = (memory().m_register[RPi3B::ClockRegistersAddress::CM_GP0CTL] >> 10) & (~(~0U << 2));
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::ALL:
        {
            value = memory().m_register[RPi3B::ClockRegistersAddress::CM_GP0CTL];
        }
        break;

    
    default:
        break;
    }

    return(value);
}

void CLOCK::CM_GP1CTL(RPi3B::ClockRegistersAddress::CM_GPnCTL_Type ctl, divisor_type value) {
    switch (ctl)
    {
    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::SRC:
        {
            memory().m_register[RPi3B::ClockRegistersAddress::CM_GP1CTL] |= (~(~0U << 4)) & value;
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::ENAB:
        {
            memory().m_register[RPi3B::ClockRegistersAddress::CM_GP1CTL] |= ((value & 01) << 5);
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::KILL:
        {
            memory().m_register[RPi3B::ClockRegistersAddress::CM_GP1CTL] |= ((value & 01) << 6);
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::BUSY:
        {
            /* Read only register */
        }
        break;
    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::FLIP:
        {
            /* Read only register */
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::MASH:
        {
            memory().m_register[RPi3B::ClockRegistersAddress::CM_GP1CTL] |= ((value & (~(~0U << 2))) << 10);
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::ALL:
        {
            auto all_value = 0;
            all_value = (~(~0U << 4)) & value | //SRC
                        ((value & 01) << 5) | // ENAB
                        ((value & 01) << 6) | //KILL
                        ((value & (~(~0U << 2))) << 10); //MASH
            memory().m_register[RPi3B::ClockRegistersAddress::CM_GP1CTL] = all_value;

        }
        break;

    
    default:
        break;
    }

}

CLOCK::control_type CLOCK::CM_GP1CTL(RPi3B::ClockRegistersAddress::CM_GPnCTL_Type ctlType) const {

    CLOCK::control_type value = 0;
    switch (ctlType)
    {
    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::SRC:
        {
            value = memory().m_register[RPi3B::ClockRegistersAddress::CM_GP1CTL] & (~(~0U << 4));
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::ENAB:
        {
            value = (memory().m_register[RPi3B::ClockRegistersAddress::CM_GP1CTL] >> 5) & 01;
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::KILL:
        {
            value = (memory().m_register[RPi3B::ClockRegistersAddress::CM_GP1CTL] >> 6) & 01;
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::BUSY:
        {
            /* Read only register */
        }
        break;
    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::FLIP:
        {
            /* Read only register */
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::MASH:
        {
            value = (memory().m_register[RPi3B::ClockRegistersAddress::CM_GP1CTL] >> 10) & (~(~0U << 2));
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::ALL:
        {
            value = memory().m_register[RPi3B::ClockRegistersAddress::CM_GP1CTL];
        }
        break;

    
    default:
        break;
    }

    return(value);
}

void CLOCK::CM_GP2CTL(RPi3B::ClockRegistersAddress::CM_GPnCTL_Type ctl, divisor_type value) {
    switch (ctl)
    {
    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::SRC:
        {
            memory().m_register[RPi3B::ClockRegistersAddress::CM_GP2CTL] |= (~(~0U << 4)) & value;
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::ENAB:
        {
            memory().m_register[RPi3B::ClockRegistersAddress::CM_GP2CTL] |= ((value & 01) << 5);
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::KILL:
        {
            memory().m_register[RPi3B::ClockRegistersAddress::CM_GP2CTL] |= ((value & 01) << 6);
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::BUSY:
        {
            /* Read only register */
        }
        break;
    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::FLIP:
        {
            /* Read only register */
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::MASH:
        {
            memory().m_register[RPi3B::ClockRegistersAddress::CM_GP2CTL] |= ((value & (~(~0U << 2))) << 10);
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::ALL:
        {
            auto all_value = 0;
            all_value = (~(~0U << 4)) & value | //SRC
                        ((value & 01) << 5) | // ENAB
                        ((value & 01) << 6) | //KILL
                        ((value & (~(~0U << 2))) << 10); //MASH
            memory().m_register[RPi3B::ClockRegistersAddress::CM_GP2CTL] = all_value;

        }
        break;

    
    default:
        break;
    }

}

CLOCK::control_type CLOCK::CM_GP2CTL(RPi3B::ClockRegistersAddress::CM_GPnCTL_Type ctlType) const {

    CLOCK::control_type value = 0;
    switch (ctlType)
    {
    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::SRC:
        {
            value = memory().m_register[RPi3B::ClockRegistersAddress::CM_GP2CTL] & (~(~0U << 4));
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::ENAB:
        {
            value = (memory().m_register[RPi3B::ClockRegistersAddress::CM_GP2CTL] >> 5) & 01;
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::KILL:
        {
            value = (memory().m_register[RPi3B::ClockRegistersAddress::CM_GP2CTL] >> 6) & 01;
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::BUSY:
        {
            /* Read only register */
        }
        break;
    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::FLIP:
        {
            /* Read only register */
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::MASH:
        {
            value = (memory().m_register[RPi3B::ClockRegistersAddress::CM_GP2CTL] >> 10) & (~(~0U << 2));
        }
        break;

    case RPi3B::ClockRegistersAddress::CM_GPnCTL_Type::ALL:
        {
            value = memory().m_register[RPi3B::ClockRegistersAddress::CM_GP2CTL];
        }
        break;

    
    default:
        break;
    }

    return(value);
}


void CLOCK::write(RPi3B::ClockRegistersAddress::Register reg, RPi3B::ClockRegistersAddress::CM_GPnCTL_Type type ,divisor_type value) {

    switch (reg)
    {
        case RPi3B::ClockRegistersAddress::Register::CM_GP0CTL:
        {
            CM_GP0CTL(type, value);
        }
        break;

        case RPi3B::ClockRegistersAddress::Register::CM_GP1CTL:
        {
            CM_GP1CTL(type, value);
        }
        break;

        case RPi3B::ClockRegistersAddress::Register::CM_GP2CTL:
        {
            CM_GP2CTL(type, value);
        }
        break;
    
        default:
            /// Error Handling
            break;
    }    

}

CLOCK::control_type CLOCK::read(RPi3B::ClockRegistersAddress::Register reg, RPi3B::ClockRegistersAddress::CM_GPnCTL_Type type) const {
    CLOCK::control_type value;
    switch (reg)
    {
        case RPi3B::ClockRegistersAddress::Register::CM_GP0CTL:
        {
            value = CM_GP0CTL(type);
        }
        break;

        case RPi3B::ClockRegistersAddress::Register::CM_GP1CTL:
        {
            value = CM_GP1CTL(type);
        }
        break;

        case RPi3B::ClockRegistersAddress::Register::CM_GP2CTL:
        {
            value = CM_GP2CTL(type);
        }
        break;
    
        default:
            /// Error Handling
            break;
    }
    return(value);
}


#endif /*__clock_cpp__*/