#ifndef __clock_cpp__
#define __clock_cpp__

#include "clock.hpp"

void CLOCK::set_CM_GPnDIV(BCM2837::ClockRegistersAddress::Register reg, BCM2837::ClockRegistersAddress::ClockDivisor field, divisor_type value) {

    switch (field)
    {
    case BCM2837::ClockRegistersAddress::ClockDivisor::DIVI:
        {
            /// SET bits 12...23 ---- Integer Divisor
            memory().m_register[reg] |= (((~((~0U) << 12)) & value) << 12);
        }
        break;

        case BCM2837::ClockRegistersAddress::ClockDivisor::DIVF:
        {
            /// SET bits 0...11 --- Float divisor
            memory().m_register[reg] |= ((~((~0U) << 12)) & value);
        }
        break;

        case BCM2837::ClockRegistersAddress::ClockDivisor::BOTH_VALUE:
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

void CLOCK::clr_CM_GPnDIV(BCM2837::ClockRegistersAddress::Register reg, BCM2837::ClockRegistersAddress::ClockDivisor field) {
    switch (field)
    {
    case BCM2837::ClockRegistersAddress::ClockDivisor::DIVI:
        {
            /// SET bits 12...23 ---- Integer Divisor
            memory().m_register[reg] &= ((~((~0U) << 24))  & (~((~0U) << 12)));
        }
        break;

        case BCM2837::ClockRegistersAddress::ClockDivisor::DIVF:
        {
            /// SET bits 0...11 --- Float divisor
            memory().m_register[reg] &= ((~0U) << 12);
        }
        break;

        case BCM2837::ClockRegistersAddress::ClockDivisor::BOTH_VALUE:
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

CLOCK::divisor_type CLOCK::get_CM_GPnDIV(BCM2837::ClockRegistersAddress::Register reg, BCM2837::ClockRegistersAddress::ClockDivisor field) {

    CLOCK::divisor_type value = 0;
    switch (field)
    {
    case BCM2837::ClockRegistersAddress::ClockDivisor::DIVI:
        {
            /// SET bits 12...23 ---- Integer Divisor
            value = (memory().m_register[reg] >> 12) & (~((~0U) << 12));
        }
        break;

        case BCM2837::ClockRegistersAddress::ClockDivisor::DIVF:
        {
            /// SET bits 0...11 --- Float divisor
            value = memory().m_register[reg] & (~(1U << 12));
        }
        break;

        case BCM2837::ClockRegistersAddress::ClockDivisor::BOTH_VALUE:
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

void CLOCK::set_CM_GPnCTL(BCM2837::ClockRegistersAddress::Register reg, BCM2837::ClockRegistersAddress::CM_GPnCTL_Type field, divisor_type value) {
    switch (field)
    {
    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::SRC:
        {
            memory().m_register[reg] |= (~(~0U << 4)) & value;
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::ENAB:
        {
            memory().m_register[reg] |= ((value & 01) << 4);
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::KILL:
        {
            memory().m_register[reg] |= ((value & 01) << 5);
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::BUSY:
        {
            /* Read only register */
        }
        break;
    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::FLIP:
        {
            /* Read only register */
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::MASH:
        {
            memory().m_register[reg] |= ((value & 0b11) << 8);
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::ALL:
        {
            memory().m_register[reg] = value;
        }
        break;

    
    default:
        break;
    }

}

void CLOCK::clr_CM_GPnCTL(BCM2837::ClockRegistersAddress::Register reg, BCM2837::ClockRegistersAddress::CM_GPnCTL_Type field) {
    switch (field)
    {
    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::SRC:
        {
            memory().m_register[reg] &= (~0b1111 << 0);
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::ENAB:
        {
            memory().m_register[reg] &= (~(1U << 4));
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::KILL:
        {
            memory().m_register[reg] &= (~(1U << 5));
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::BUSY:
        {
            /* Read only register */
        }
        break;
    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::FLIP:
        {
            /* Read only register */
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::MASH:
        {
            memory().m_register[reg] &= (~(0b11 << 8));
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::ALL:
        {
            memory().m_register[reg] &= (~(0b1111 | (0b1 << 4) | (0b1 << 5) | (01 << 6) | (0b1 << 7) | (0b11 << 8)));
        }
        break;

    
    default:
        break;
    }
}

CLOCK::control_type CLOCK::get_CM_GPnCTL(BCM2837::ClockRegistersAddress::Register reg, BCM2837::ClockRegistersAddress::CM_GPnCTL_Type field) {
    CLOCK::control_type value = 0;
    switch (field)
    {
    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::SRC:
        {
            value = memory().m_register[reg] & 0b1111;
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::ENAB:
        {
            value = (memory().m_register[reg] >> 4) & 01;
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::KILL:
        {
            value = (memory().m_register[reg] >> 5) & 01;
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::BUSY:
        {
            /* Read only register */
        }
        break;
    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::FLIP:
        {
            /* Read only register */
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::MASH:
        {
            value = (memory().m_register[reg] >> 8) & 0b11;
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::ALL:
        {
            value = memory().m_register[reg];
        }
        break;

    
    default:
        break;
    }

    return(value);
}


void CLOCK::CM_GP0DIV(BCM2837::ClockRegistersAddress::ClockDivisor divisor, CLOCK::divisor_type value) {

    switch (divisor)
    {
        case BCM2837::ClockRegistersAddress::ClockDivisor::DIVI:
        {
            /// SET bits 12...23 ---- Integer Divisor
            memory().m_register[BCM2837::ClockRegistersAddress::Register::CM_GP0DIV] |= (((~((~0U) << 12)) & value) << 12);
        }
        break;

        case BCM2837::ClockRegistersAddress::ClockDivisor::DIVF:
        {
            /// SET bits 0...11 --- Float divisor
            memory().m_register[BCM2837::ClockRegistersAddress::Register::CM_GP0DIV] |= ((~((~0U) << 12)) & value);
        }
        break;

        case BCM2837::ClockRegistersAddress::ClockDivisor::BOTH_VALUE:
        {
            /// SET bits 0...23 --- both Integer & Float divisor
            memory().m_register[BCM2837::ClockRegistersAddress::Register::CM_GP0DIV] |= ((~((~0U) << 24)) & value);
        }
        break;
    
        default:
            /// Error Handling
            break;
    }
}

CLOCK::divisor_type CLOCK::CM_GP0DIV(BCM2837::ClockRegistersAddress::ClockDivisor divisor) const {
    divisor_type value = 0;

    switch (divisor)
    {
        case BCM2837::ClockRegistersAddress::ClockDivisor::DIVI:
        {
            /// SET bits 12...23 ---- Integer Divisor
            value = (memory().m_register[BCM2837::ClockRegistersAddress::Register::CM_GP0DIV] >> 12) & (~((~0U) << 12));
        }
        break;

        case BCM2837::ClockRegistersAddress::ClockDivisor::DIVF:
        {
            /// SET bits 0...11 --- Float divisor
            value = memory().m_register[BCM2837::ClockRegistersAddress::Register::CM_GP0DIV] & (~((~0U) << 12U));
        }
        break;

        case BCM2837::ClockRegistersAddress::ClockDivisor::BOTH_VALUE:
        {
            /// SET bits 0...23 --- both Integer & Float divisor
            value = memory().m_register[BCM2837::ClockRegistersAddress::Register::CM_GP0DIV] & (~((~0U) << 24U));
        }
        break;
    
        default:
            /// Error Handling
            break;
    }
    return(value);
}

void CLOCK::CM_GP1DIV(BCM2837::ClockRegistersAddress::ClockDivisor divisor, CLOCK::divisor_type value) {
    switch (divisor)
    {
        case BCM2837::ClockRegistersAddress::ClockDivisor::DIVI:
        {
            /// SET bits 12...23 ---- Integer Divisor
            memory().m_register[BCM2837::ClockRegistersAddress::Register::CM_GP1DIV] |= (((~((~0U) << 12)) & value) << 12);
        }
        break;

        case BCM2837::ClockRegistersAddress::ClockDivisor::DIVF:
        {
            /// SET bits 0...11 --- Float divisor
            memory().m_register[BCM2837::ClockRegistersAddress::Register::CM_GP1DIV] |= (~((~0U) << 12) & value);
        }
        break;

        case BCM2837::ClockRegistersAddress::ClockDivisor::BOTH_VALUE:
        {
            /// SET bits 0...23 --- both Integer & Float divisor
            memory().m_register[BCM2837::ClockRegistersAddress::Register::CM_GP1DIV] |= (~((~0U) << 24) & value);
        }
        break;
    
        default:
            /// Error Handling
            break;
    }
}

CLOCK::divisor_type CLOCK::CM_GP1DIV(BCM2837::ClockRegistersAddress::ClockDivisor divisor) const {
    divisor_type value = 0;

    switch (divisor)
    {
        case BCM2837::ClockRegistersAddress::ClockDivisor::DIVI:
        {
            /// SET bits 12...23 ---- Integer Divisor
            value = (memory().m_register[BCM2837::ClockRegistersAddress::Register::CM_GP1DIV] >> 12) & (~((~0U) << 12));
        }
        break;

        case BCM2837::ClockRegistersAddress::ClockDivisor::DIVF:
        {
            /// SET bits 0...11 --- Float divisor
            value = memory().m_register[BCM2837::ClockRegistersAddress::Register::CM_GP1DIV] & (~((~0U) << 12));
        }
        break;

        case BCM2837::ClockRegistersAddress::ClockDivisor::BOTH_VALUE:
        {
            /// SET bits 0...23 --- both Integer & Float divisor
            value = memory().m_register[BCM2837::ClockRegistersAddress::Register::CM_GP1DIV] & (~((~0U) << 24));
        }
        break;
    
        default:
            /// Error Handling
            break;
    }
    return(value);
}

void CLOCK::CM_GP2DIV(BCM2837::ClockRegistersAddress::ClockDivisor divisor, CLOCK::divisor_type value) {
    switch (divisor)
    {
        case BCM2837::ClockRegistersAddress::ClockDivisor::DIVI:
        {
            /// SET bits 12...23 ---- Integer Divisor
            memory().m_register[BCM2837::ClockRegistersAddress::Register::CM_GP2DIV] |= ((~((~0U) << 12) & value) << 12);
        }
        break;

        case BCM2837::ClockRegistersAddress::ClockDivisor::DIVF:
        {
            /// SET bits 0...11 --- Float divisor
            memory().m_register[BCM2837::ClockRegistersAddress::Register::CM_GP2DIV] |= (~((~0U) << 12) & value);
        }
        break;

        case BCM2837::ClockRegistersAddress::ClockDivisor::BOTH_VALUE:
        {
            /// SET bits 0...23 --- both Integer & Float divisor
            memory().m_register[BCM2837::ClockRegistersAddress::Register::CM_GP2DIV] |= (~((~0U) << 24) & value);
        }
        break;
    
        default:
            /// Error Handling
            break;
    }
}

CLOCK::divisor_type CLOCK::CM_GP2DIV(BCM2837::ClockRegistersAddress::ClockDivisor divisor) const {
    divisor_type value = 0;

    switch (divisor)
    {
        case BCM2837::ClockRegistersAddress::ClockDivisor::DIVI:
        {
            /// SET bits 12...23 ---- Integer Divisor
            value = (memory().m_register[BCM2837::ClockRegistersAddress::Register::CM_GP2DIV] >> 12) & (~((~0U) << 12));
        }
        break;

        case BCM2837::ClockRegistersAddress::ClockDivisor::DIVF:
        {
            /// SET bits 0...11 --- Float divisor
            value = memory().m_register[BCM2837::ClockRegistersAddress::Register::CM_GP2DIV] & (~((~0U) << 12));
        }
        break;

        case BCM2837::ClockRegistersAddress::ClockDivisor::BOTH_VALUE:
        {
            /// SET bits 0...23 --- both Integer & Float divisor
            value = memory().m_register[BCM2837::ClockRegistersAddress::Register::CM_GP2DIV] & (~((~0U) << 24));
        }
        break;
    
        default:
            /// Error Handling
            break;
    }
    return(value);
}

void CLOCK::write(BCM2837::ClockRegistersAddress::Register reg, BCM2837::ClockRegistersAddress::ClockDivisor divif, CLOCK::divisor_type value) {

    switch (reg)
    {
        case BCM2837::ClockRegistersAddress::Register::CM_GP0DIV:
        {
            CM_GP0DIV(divif, value);
        }
        break;

        case BCM2837::ClockRegistersAddress::Register::CM_GP1DIV:
        {
            CM_GP1DIV(divif, value);
        }
        break;

        case BCM2837::ClockRegistersAddress::Register::CM_GP2DIV:
        {
            CM_GP2DIV(divif, value);
        }
        break;
    
        default:
            /// Error Handling
            break;
    }
}

CLOCK::divisor_type CLOCK::read(BCM2837::ClockRegistersAddress::Register reg, BCM2837::ClockRegistersAddress::ClockDivisor divif) const {

    CLOCK::divisor_type value = 0;
    switch (reg)
    {
        case BCM2837::ClockRegistersAddress::Register::CM_GP0DIV:
        {
            value = CM_GP0DIV(divif);
        }
        break;

        case BCM2837::ClockRegistersAddress::Register::CM_GP1DIV:
        {
            value = CM_GP1DIV(divif);
        }
        break;

        case BCM2837::ClockRegistersAddress::Register::CM_GP2DIV:
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

void CLOCK::CM_GP0CTL(BCM2837::ClockRegistersAddress::CM_GPnCTL_Type ctl, divisor_type value) {
    switch (ctl)
    {
    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::SRC:
        {
            memory().m_register[BCM2837::ClockRegistersAddress::CM_GP0CTL] |= (~(~0U << 4)) & value;
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::ENAB:
        {
            memory().m_register[BCM2837::ClockRegistersAddress::CM_GP0CTL] |= ((value & 01) << 5);
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::KILL:
        {
            memory().m_register[BCM2837::ClockRegistersAddress::CM_GP0CTL] |= ((value & 01) << 6);
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::BUSY:
        {
            /* Read only register */
        }
        break;
    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::FLIP:
        {
            /* Read only register */
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::MASH:
        {
            memory().m_register[BCM2837::ClockRegistersAddress::CM_GP0CTL] |= ((value & (~(~0U << 2))) << 10);
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::ALL:
        {
            auto all_value = 0;
            all_value = (~(~0U << 4)) & value | //SRC
                        ((value & 01) << 5) | // ENAB
                        ((value & 01) << 6) | //KILL
                        ((value & (~(~0U << 2))) << 10); //MASH
            memory().m_register[BCM2837::ClockRegistersAddress::CM_GP0CTL] = all_value;

        }
        break;

    
    default:
        break;
    }

}

CLOCK::control_type CLOCK::CM_GP0CTL(BCM2837::ClockRegistersAddress::CM_GPnCTL_Type ctlType) const {
    
    CLOCK::control_type value = 0;
    switch (ctlType)
    {
    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::SRC:
        {
            value = memory().m_register[BCM2837::ClockRegistersAddress::CM_GP0CTL] & (~(~0U << 4));
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::ENAB:
        {
            value = (memory().m_register[BCM2837::ClockRegistersAddress::CM_GP0CTL] >> 5) & 01;
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::KILL:
        {
            value = (memory().m_register[BCM2837::ClockRegistersAddress::CM_GP0CTL] >> 6) & 01;
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::BUSY:
        {
            /* Read only register */
        }
        break;
    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::FLIP:
        {
            /* Read only register */
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::MASH:
        {
            value = (memory().m_register[BCM2837::ClockRegistersAddress::CM_GP0CTL] >> 10) & (~(~0U << 2));
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::ALL:
        {
            value = memory().m_register[BCM2837::ClockRegistersAddress::CM_GP0CTL];
        }
        break;

    
    default:
        break;
    }

    return(value);
}

void CLOCK::CM_GP1CTL(BCM2837::ClockRegistersAddress::CM_GPnCTL_Type ctl, divisor_type value) {
    switch (ctl)
    {
    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::SRC:
        {
            memory().m_register[BCM2837::ClockRegistersAddress::CM_GP1CTL] |= (~(~0U << 4)) & value;
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::ENAB:
        {
            memory().m_register[BCM2837::ClockRegistersAddress::CM_GP1CTL] |= ((value & 01) << 5);
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::KILL:
        {
            memory().m_register[BCM2837::ClockRegistersAddress::CM_GP1CTL] |= ((value & 01) << 6);
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::BUSY:
        {
            /* Read only register */
        }
        break;
    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::FLIP:
        {
            /* Read only register */
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::MASH:
        {
            memory().m_register[BCM2837::ClockRegistersAddress::CM_GP1CTL] |= ((value & (~(~0U << 2))) << 10);
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::ALL:
        {
            auto all_value = 0;
            all_value = (~(~0U << 4)) & value | //SRC
                        ((value & 01) << 5) | // ENAB
                        ((value & 01) << 6) | //KILL
                        ((value & (~(~0U << 2))) << 10); //MASH
            memory().m_register[BCM2837::ClockRegistersAddress::CM_GP1CTL] = all_value;

        }
        break;

    
    default:
        break;
    }

}

CLOCK::control_type CLOCK::CM_GP1CTL(BCM2837::ClockRegistersAddress::CM_GPnCTL_Type ctlType) const {

    CLOCK::control_type value = 0;
    switch (ctlType)
    {
    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::SRC:
        {
            value = memory().m_register[BCM2837::ClockRegistersAddress::CM_GP1CTL] & (~(~0U << 4));
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::ENAB:
        {
            value = (memory().m_register[BCM2837::ClockRegistersAddress::CM_GP1CTL] >> 5) & 01;
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::KILL:
        {
            value = (memory().m_register[BCM2837::ClockRegistersAddress::CM_GP1CTL] >> 6) & 01;
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::BUSY:
        {
            /* Read only register */
        }
        break;
    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::FLIP:
        {
            /* Read only register */
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::MASH:
        {
            value = (memory().m_register[BCM2837::ClockRegistersAddress::CM_GP1CTL] >> 10) & (~(~0U << 2));
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::ALL:
        {
            value = memory().m_register[BCM2837::ClockRegistersAddress::CM_GP1CTL];
        }
        break;

    
    default:
        break;
    }

    return(value);
}

void CLOCK::CM_GP2CTL(BCM2837::ClockRegistersAddress::CM_GPnCTL_Type ctl, divisor_type value) {
    switch (ctl)
    {
    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::SRC:
        {
            memory().m_register[BCM2837::ClockRegistersAddress::CM_GP2CTL] |= (~(~0U << 4)) & value;
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::ENAB:
        {
            memory().m_register[BCM2837::ClockRegistersAddress::CM_GP2CTL] |= ((value & 01) << 5);
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::KILL:
        {
            memory().m_register[BCM2837::ClockRegistersAddress::CM_GP2CTL] |= ((value & 01) << 6);
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::BUSY:
        {
            /* Read only register */
        }
        break;
    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::FLIP:
        {
            /* Read only register */
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::MASH:
        {
            memory().m_register[BCM2837::ClockRegistersAddress::CM_GP2CTL] |= ((value & (~(~0U << 2))) << 10);
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::ALL:
        {
            auto all_value = 0;
            all_value = (~(~0U << 4)) & value | //SRC
                        ((value & 01) << 5) | // ENAB
                        ((value & 01) << 6) | //KILL
                        ((value & (~(~0U << 2))) << 10); //MASH
            memory().m_register[BCM2837::ClockRegistersAddress::CM_GP2CTL] = all_value;

        }
        break;

    
    default:
        break;
    }

}

CLOCK::control_type CLOCK::CM_GP2CTL(BCM2837::ClockRegistersAddress::CM_GPnCTL_Type ctlType) const {

    CLOCK::control_type value = 0;
    switch (ctlType)
    {
    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::SRC:
        {
            value = memory().m_register[BCM2837::ClockRegistersAddress::CM_GP2CTL] & (~(~0U << 4));
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::ENAB:
        {
            value = (memory().m_register[BCM2837::ClockRegistersAddress::CM_GP2CTL] >> 5) & 01;
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::KILL:
        {
            value = (memory().m_register[BCM2837::ClockRegistersAddress::CM_GP2CTL] >> 6) & 01;
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::BUSY:
        {
            /* Read only register */
        }
        break;
    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::FLIP:
        {
            /* Read only register */
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::MASH:
        {
            value = (memory().m_register[BCM2837::ClockRegistersAddress::CM_GP2CTL] >> 10) & (~(~0U << 2));
        }
        break;

    case BCM2837::ClockRegistersAddress::CM_GPnCTL_Type::ALL:
        {
            value = memory().m_register[BCM2837::ClockRegistersAddress::CM_GP2CTL];
        }
        break;

    
    default:
        break;
    }

    return(value);
}


void CLOCK::write(BCM2837::ClockRegistersAddress::Register reg, BCM2837::ClockRegistersAddress::CM_GPnCTL_Type type ,divisor_type value) {

    switch (reg)
    {
        case BCM2837::ClockRegistersAddress::Register::CM_GP0CTL:
        {
            CM_GP0CTL(type, value);
        }
        break;

        case BCM2837::ClockRegistersAddress::Register::CM_GP1CTL:
        {
            CM_GP1CTL(type, value);
        }
        break;

        case BCM2837::ClockRegistersAddress::Register::CM_GP2CTL:
        {
            CM_GP2CTL(type, value);
        }
        break;
    
        default:
            /// Error Handling
            break;
    }    

}

CLOCK::control_type CLOCK::read(BCM2837::ClockRegistersAddress::Register reg, BCM2837::ClockRegistersAddress::CM_GPnCTL_Type type) const {
    CLOCK::control_type value;
    switch (reg)
    {
        case BCM2837::ClockRegistersAddress::Register::CM_GP0CTL:
        {
            value = CM_GP0CTL(type);
        }
        break;

        case BCM2837::ClockRegistersAddress::Register::CM_GP1CTL:
        {
            value = CM_GP1CTL(type);
        }
        break;

        case BCM2837::ClockRegistersAddress::Register::CM_GP2CTL:
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