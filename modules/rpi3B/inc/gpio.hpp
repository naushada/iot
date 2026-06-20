#ifndef __gpio_hpp__
#define __gpio_hpp__

#include <cstdint>
#include <cstdio>
#include <atomic>

#include "memory_map.hpp"

class GPIO {
    public:
        using gpio_number = std::uint32_t;
        using pin_number = std::uint32_t;

        GPIO() : m_memory(*new RPi3B::GPIORegistersAddress) {}

        template<typename Region>
        GPIO(Region region) : m_memory(*new(region) RPi3B::GPIORegistersAddress) {}

        ~GPIO() = default;

        /**
         * @brief
            * 000 = GPIO Pin n is an input
            * 001 = GPIO Pin n is an output
            * 100 = GPIO Pin n takes alternate function 0
            * 101 = GPIO Pin n takes alternate function 1
            * 110 = GPIO Pin n takes alternate function 2
            * 111 = GPIO Pin n takes alternate function 3
            * 011 = GPIO Pin n takes alternate function 4
            * 010 = GPIO Pin n takes alternate function 5
            * 
         * @param gpio number
         * @return none
         **/
        void input(gpio_number gpio);
        void output(gpio_number gpio);
        /**
         * @brief
         *      The pin level registers return the actual value of the pin. The LEV{n} field gives the
         *      value of the respective GPIO pin.
         * @param gpio_no is the GPIO number
         * @return current LEVn valueof that GPIO number
        */
        std::uint32_t GPLEVn(gpio_number gpio) const;
        void GPLEVn(gpio_number gpio_n);

        /**
         * @brief
            * The output clear registers are used to clear a GPIO pin. 
            * The CLR{n} field defines the respective GPIO pin to clear, 
            * writing a “0” to the field has no effect. 
            * If the GPIO pin is being used as in input (by default) 
            * then the value in the CLR{n} field is ignored. However, 
            * if the pin is subsequently defined as an output then the bit will be set according to 
            * the last set/clear operation. 
            * Separating the set and clear functions removes the need for read-modify-write operations.
            *
         * @param gpio_n This is GPIO number to be cleared
         * @return none
         * */
        void GPCLRn(gpio_number gpio_n);
        /**
         * @brief
         *   The output set registers are used to set a GPIO pin. The SET{n} field defines the
         *   respective GPIO pin to set, writing a “0” to the field has no effect. If the GPIO pin is
         *   being used as in input (by default) then the value in the SET{n} field is ignored.
         *   However, if the pin is subsequently defined as an output then the bit will be set
         *   according to the last set/clear operation. Separating the set and clear functions
         *   removes the need for read-modify-write operations
         * @param gpio_n This is GPIO number which is to be set
         * @return none
        */
        void GPSETn(gpio_number gpio_n);
        std::uint32_t GPGETn(gpio_number gpio_n);
        /**
         * @brief
         *      The event detect status registers are used to record level and edge events on the
                GPIO pins. The relevant bit in the event detect status registers is set whenever: 1)
                an edge is detected that matches the type of edge programmed in the rising/falling
                edge detect enable registers, or 2) a level is detected that matches the type of level
                programmed in the high/low level detect enable registers. The bit is cleared by
                writing a “1” to the relevant bit.
         * @param
         * @return 
        */
        void GPEDSn(gpio_number gpio_n);
        std::uint32_t GPEDSn(gpio_number gpio_n) const;
        /**
         * @brief
         *      The rising edge detect enable registers define the pins for which a rising edge
                transition sets a bit in the event detect status registers (GPEDSn). When the
                relevant bits are set in both the GPRENn and GPFENn registers, any transition (1
                to 0 and 0 to 1) will set a bit in the GPEDSn registers. The GPRENn registers use
                synchronous edge detection. This means the input signal is sampled using the
                system clock and then it is looking for a “011” pattern on the sampled signal. This
                has the effect of suppressing glitches.
         * @param
         * @return
        */
        void GPRENn(gpio_number gpio_n);
        std::uint32_t GPRENn(gpio_number gpio_n) const;

        /**
         * @brief
         *      The falling edge detect enable registers define the pins for which a falling edge
                transition sets a bit in the event detect status registers (GPEDSn). When the relevant
                bits are set in both the GPRENn and GPFENn registers, any transition (1 to 0 and 0
                to 1) will set a bit in the GPEDSn registers. The GPFENn registers use synchronous
                edge detection. This means the input signal is sampled using the system clock and
                then it is looking for a “100” pattern on the sampled signal. This has the effect of
                suppressing glitches.
         * @param
         * @return
        */
        void GPFENn(gpio_number gpio_n);
        std::uint32_t GPFENn(gpio_number gpio_n) const;

        /**
         * @brief
         *      The high level detect enable registers define the pins for which a high level sets a bit in
                the event detect status register (GPEDSn). If the pin is still high when an attempt is
                made to clear the status bit in GPEDSn then the status bit will remain set.
         * @param
         * @return
        */
        void GPHENn(gpio_number gpio_n);
        std::uint32_t GPHENn(gpio_number gpio_n) const;

        /**
         * @brief
         *      The low level detect enable registers define the pins for which a low level sets a bit in
                the event detect status register (GPEDSn). If the pin is still low when an attempt is
                made to clear the status bit in GPEDSn then the status bit will remain set.
         * @param
         * @return
        */
        void GPLENn(gpio_number gpio_n);
        std::uint32_t GPLENn(gpio_number gpio_n) const;

        /**
         * @brief
         *      The asynchronous rising edge detect enable registers define the pins for which a
                asynchronous rising edge transition sets a bit in the event detect status registers
                (GPEDSn).
                Asynchronous means the incoming signal is not sampled by the system clock. As such
                rising edges of very short duration can be detected.
         *  @param
         *  @return
        */
        void GPARENn(gpio_number gpio_n);
        std::uint32_t GPARENn(gpio_number gpio_n) const;

        /**
         * @brief
         *      The asynchronous falling edge detect enable registers define the pins for which a
                asynchronous falling edge transition sets a bit in the event detect status registers
                (GPEDSn). Asynchronous means the incoming signal is not sampled by the system
                clock. As such falling edges of very short duration can be detected.
         * @param
         * @return
        */
        void GPAFENn(gpio_number gpio_n);
        std::uint32_t GPAFENn(gpio_number gpio_n) const;

        /**
         * @brief
         *      The GPIO Pull-up/down Register controls the actuation of the internal pull-up/down
                control line to ALL the GPIO pins. This register must be used in conjunction with the 2
                GPPUDCLKn registers.
                Note that it is not possible to read back the current Pull-up/down settings and so it is the
                users’ responsibility to ‘remember’ which pull-up/downs are active. The reason for this is
                that GPIO pull-ups are maintained even in power-down mode when the core is off, when
                all register contents is lost.
                The Alternate function table also has the pull state which is applied after a power down.
         * @param
         * @return
         *      
        */
        void GPPUD(gpio_number gpio_n, RPi3B::GPIORegistersAddress::PullUpDownConfig cfg);
        std::uint32_t GPPUD(gpio_number gpio_n) const;

        /**
         * @brief
         *      The GPIO Pull-up/down Clock Registers control the actuation of internal pull-downs on
                the respective GPIO pins. These registers must be used in conjunction with the GPPUD
                register to effect GPIO Pull-up/down changes. The following sequence of events is
                required:
                    1. Write to GPPUD to set the required control signal (i.e. Pull-up or Pull-Down or neither
                        to remove the current Pull-up/down)
                    2. Wait 150 cycles – this provides the required set-up time for the control signal
                    3. Write to GPPUDCLK0/1 to clock the control signal into the GPIO pads you wish to
                        modify – NOTE only the pads which receive a clock will be modified, all others will
                        retain their previous state.
                    4. Wait 150 cycles – this provides the required hold time for the control signal
                    5. Write to GPPUD to remove the control signal
                    6. Write to GPPUDCLK0/1 to remove the clock
         * @param
         * @return
         *      
        */
        void GPPUDCLKn(gpio_number gpio_n);
        std::uint32_t GPPUDCLKn(gpio_number gpio_n) const;

        std::uint32_t read(gpio_number gpio_n);
        std::uint32_t read32(gpio_number gpio_n);
        void write32(gpio_number gpio_n, std::uint32_t value);
        void write(gpio_number gpio_n, RPi3B::GPIORegistersAddress::Config cfg);

        RPi3B::GPIORegistersAddress& memory() const {
            return(m_memory);
        }

    private:
        RPi3B::GPIORegistersAddress& m_memory;

};





























#endif /* __gpio_hpp__*/