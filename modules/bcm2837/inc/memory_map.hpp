#ifndef __memory_map_hpp__
#define __memory_map_hpp__

#include <cstdint>
#include <cstdio>

namespace BCM2837 {

    /**
     * @brief One 32-bit memory-mapped I/O register.
     *
     * Plain `volatile` storage — NOT `std::atomic`. For a word-aligned MMIO
     * register a volatile load/store is the correct primitive; atomics add no
     * value over the peripheral bus (see docs/DRIVER_REVIEW.md §2.1). Two
     * properties matter for the real-hardware path:
     *
     *  1. **Trivially default-constructible.** No user-declared constructor, so
     *     placement-new'ing a register block over *live* MMIO does NOT run any
     *     element constructor and therefore does NOT clobber the registers.
     *     (C++20 `std::atomic`'s default constructor value-initialises to 0,
     *     which would zero every register on attach.)
     *  2. **No deprecated volatile compound-assignment.** The compound operators
     *     below do an explicit volatile load / modify / store, so callers can
     *     keep writing `reg |= bit;` without tripping C++20's `-Wdeprecated-
     *     volatile` on `volatile`-qualified `|=`/`&=`.
     *
     * Layout-compatible with `uint32_t` (standard-layout, size/align 4), so the
     * unit tests can still overlay a `std::vector<std::uint32_t>` region.
    */
    struct mmio_reg {
        volatile std::uint32_t value;

        operator std::uint32_t() const { return value; }     ///< volatile read
        std::uint32_t load() const { return value; }
        void store(std::uint32_t v) { value = v; }

        mmio_reg& operator=(std::uint32_t v)  { value = v; return *this; }
        mmio_reg& operator|=(std::uint32_t v) { value = value | v; return *this; }
        mmio_reg& operator&=(std::uint32_t v) { value = value & v; return *this; }
        mmio_reg& operator^=(std::uint32_t v) { value = value ^ v; return *this; }
    };

    struct GPIORegistersAddress {

        enum Config: std::uint32_t {
            InputMode = 0b000,
            OutputMode = 0b001,
            AlternateFunction0 = 0b100,
            AlternateFunction1 = 0b101,
            AlternateFunction2 = 0b110,
            AlternateFunction3 = 0b111,
            AlternateFunction4 = 0b011,
            AlternateFunction5 = 0b010
        };

        enum PullUpDownConfig: std::uint32_t {
            Off = 0b00,
            EnablePullDown = 0b01,
            EnablePullUp = 0b10,
            Reserved = 0b11
        };

        enum Register: std::uint32_t {
            /* GPIO Function Selection offset */
            BCM2837_GPFSEL0,
            BCM2837_GPFSEL1,
            BCM2837_GPFSEL2,
            BCM2837_GPFSEL3,
            BCM2837_GPFSEL4,
            BCM2837_GPFSEL5,
            BCM2837_GPFSEL_RESERVED,
            /* GPIO SET Address offset */
            BCM2837_GPSET0,
            BCM2837_GPSET1,
            BCM2837_GPSET_RESERVED,
            /* Offset for clearing the GPIO */
            BCM2837_GPCLR0,
            BCM2837_GPCLR1,
            BCM2837_GPCLR_RESERVED,
            /* Offset for GPIO LEV0/1 */
            BCM2837_GPLEV0,
            BCM2837_GPLEV1,
            BCM2837_GPLEV_RESERVED,
            BCM2837_GPEDS0,
            BCM2837_GPEDS1,
            BCM2837_GPEDS_RESERVED,
            BCM2837_GPREN0,
            BCM2837_GPREN1,
            BCM2837_GPREN_RESERVED,
            BCM2837_GPFEN0,
            BCM2837_GPFEN1,
            BCM2837_GPFEN_RESERVED,
            BCM2837_GPHEN0,
            BCM2837_GPHEN1,
            BCM2837_GPHEN_RESERVED,
            BCM2837_GPLEN0,
            BCM2837_GPLEN1,
            BCM2837_GPLEN_RESERVED,
            BCM2837_GPAREN0,
            BCM2837_GPAREN1,
            BCM2837_GPAREN_RESERVED,
            BCM2837_GPAFEN0,
            BCM2837_GPAFEN1,
            BCM2837_GPAFEN_RESERVED,
            BCM2837_GPPUD,
            BCM2837_GPPUDCLK0,
            BCM2837_GPPUDCLK1,
            BCM2837_RESERVED,
            BCM2837_TEST1,
            BCM2837_TEST2,
            BCM2837_TEST3,
            BCM2837_TEST4,
            BCM2837_MAX
        };
    
        using device_register = mmio_reg; //@brief One MMIO register (see BCM2837::mmio_reg)
        GPIORegistersAddress() {}
        ~GPIORegistersAddress() {}

        /** 
         * @brief Compiler give preference to this new operator over global new operator and invoke this new operator. 
         * @param nBytes Number of bytes to be allocated
         * @param region is the memory region to be return if present.
         * @return pointer to void
         */
        void *operator new(std::size_t nBytes, void *region=nullptr) {
            (void)nBytes;
            if(nullptr == region) {
                /**
                 * @brief
                 *      Physical addresses range from 0x3F000000 to 0x3FFFFFFF for peripherals. The
                        bus addresses for peripherals are set up to map onto the peripheral bus address range
                        starting at 0x7E000000. Thus a peripheral advertised here at bus address 0x7Ennnnnn is
                        available at physical address 0x3Fnnnnnn.
                */
                return reinterpret_cast<void *>((0x3F000000) + (0x00200000));
            }
            return(region);
        }

        /** 
         * @brief
         * address for m_register will be 
         * [0x3F200000, 0x3F200004, 0x3F200008, 0x3F20000C, 0x3F200010, 0x3F200014, 0x3F200018, 0x3F20001C  ... 0x3F2000B0] 
         **/
        device_register m_register[Register::BCM2837_MAX];
    };

    struct ClockRegistersAddress {

        enum Register : std::uint32_t {
            CM_GP0CTL,
            CM_GP0DIV,
            CM_GP1CTL,
            CM_GP1DIV,
            CM_GP2CTL,
            CM_GP2DIV,
            CM_GPn_MAX
        };
        
        enum CM_GPnCTL_Type: std::uint32_t {
            SRC,
            ENAB,
            KILL,
            Unused,
            BUSY,
            FLIP,
            MASH,
            ALL
        };

        enum ClockSource: std::uint32_t {
            Ground = 0,
            Oscillator = 1,
            Testdebug0 = 2,
            Testdebug1 = 3,
            PLLA = 4,
            PLLC = 5,
            PLLD = 6,
            HDMI_Auxiliary = 7
            /**
             * @brief
                    8-15 = GND
                    To avoid lock-ups and glitches do not change this
                    control while BUSY=1 and do not change this control
                    at the same time as asserting ENAB
            */
        };

        enum ClockMASH : std::uint32_t {
            Integer_division = 0,
            Stage1_MASH = 1,
            Stage2_MASH = 2,
            Stage3_MASH = 3
            /**
             * @brief
             *      To avoid lock-ups and glitches do not change this
                    control while BUSY=1 and do not change this control
                    at the same time as asserting ENAB.
            */
        };

        enum ClockDivisor: std::uint32_t {
            /**
             * @brief
             *      Fractional part of divisor
                    To avoid lock-ups and glitches do not change this
                    control while BUSY=1.
            */
            DIVF,
            /**
             * @brief
             *      Integer part of divisor
                    This value has a minimum limit determined by the
                    MASH setting. See text for details. To avoid lock-ups
                    and glitches do not change this control while BUSY=1.
            */
            DIVI,
            /**
             * @brief
             *      Use BOTH_VALUE to write both Integer & Fractional part
            */
            BOTH_VALUE
        };

        using device_register = mmio_reg; //@brief One MMIO register (see BCM2837::mmio_reg)
        ClockRegistersAddress() {
            /*
            for(auto idx = 0; idx < Register::CM_GPn_MAX; ++idx)
                std::printf("Clock Register is 0x%X\n", &m_register[idx]);
            std::printf("\n");
            */
        }
        ~ClockRegistersAddress() {}

        /** 
         * @brief Compiler give preference to this new operator over global new operator and invoke this new operator. 
         * @param nBytes Number of bytes to be allocated
         * @param region is the memory region to be return if present.
         * @return pointer to void
         */
        void *operator new(std::size_t nBytes, void *region=nullptr) {
            (void)nBytes;
            if(nullptr == region) {
                return reinterpret_cast<void *>((0x3F000000) + (0x00100000));
            }
            return(region);
        }

        /** 
         * @brief
         * address for m_register will be 
         * [0x3F100000, 0x3F100004, 0x3F100008, 0x3F10000C, 0x3F100010, 0x3F100014, 0x3F100018, 0x3F10001C  ... 0x3F1000B0] 
         **/
        device_register m_register[Register::CM_GPn_MAX];
    };

    struct InterruptRegisterAddress {
        enum Register : std::uint32_t {
            IRQ_Basic_Pending,
            IRQ_Pending1,
            IRQ_Pending2,
            FIQ_Control,
            Enable_IRQs_1,
            Enable_IRQs_2,
            Enable_Basic_IRQs,
            Disable_IRQs_1,
            Disable_IRQs_2,
            Disable_Basic_IRQs,
            IRQs_ALL_MAX
        };

        
        using device_register = mmio_reg; //@brief One MMIO register (see BCM2837::mmio_reg)
        InterruptRegisterAddress() {
            for(auto idx = 0; idx < Register::IRQs_ALL_MAX; ++idx)
                std::printf("Interrupt Register is 0x%X\n", &m_register[idx]);
            std::printf("\n");
        }
        ~InterruptRegisterAddress() {}

        /** 
         * @brief Compiler give preference to this new operator over global new operator and invoke this new operator. 
         * @param nBytes Number of bytes to be allocated
         * @param region is the memory region to be return if present.
         * @return pointer to void
         */
        void *operator new(std::size_t nBytes, void *region=nullptr) {
            (void)nBytes;
            if(nullptr == region) {
                return reinterpret_cast<void *>(0x3F00B200);
            }
            return(region);
        }

        device_register* operator[](Register offset) {
            return(&m_register[offset]);
        }

        /** 
         * @brief
         * address for m_register will be 
         * [0x3F00B200, 0x3F00B204, 0x3F00B208, 0x3F00B20C, 0x3F00B210, 0x3F00B214, 0x3F00B218, 0x3F00B21C  ... 0x3F00B220, 0x3F00B224] 
         **/
        device_register m_register[Register::IRQs_ALL_MAX];

    };

    /**
     * @brief
     *      Broadcom Serial Controller (BSC) register block — the BCM2837 I2C
     *      master. Refer Section 3 "BSC" of BCM2837-ARM-Peripherals.pdf.
     *      The Pi exposes BSC1 on the 40-pin header (GPIO2=SDA1, GPIO3=SCL1,
     *      ALT0). BSC0 is at 0x3F205000, BSC1 at 0x3F804000 (bus 0x7E804000),
     *      BSC2 (0x3F805000) is reserved for HDMI. Registers are spaced 4 bytes
     *      apart, indexed directly by datasheet word offset (no pin mapping).
    */
    struct BSCRegistersAddress {

        enum Register : std::uint32_t {
            C,      /* 0x00 Control                */
            S,      /* 0x04 Status                 */
            DLEN,   /* 0x08 Data Length            */
            A,      /* 0x0C Slave Address          */
            FIFO,   /* 0x10 Data FIFO              */
            DIV,    /* 0x14 Clock Divider          */
            DEL,    /* 0x18 Data Delay             */
            CLKT,   /* 0x1C Clock Stretch Timeout  */
            BSC_MAX
        };

        /// @brief Fields of the Control register (C).
        enum Control : std::uint32_t {
            READ,   /* bit0     : 1=Read transfer, 0=Write transfer */
            CLEAR,  /* bits[5:4]: write 0b01/0b10 to clear the FIFO  */
            ST,     /* bit7     : Start a new transfer               */
            INTD,   /* bit8     : Interrupt on DONE                  */
            INTT,   /* bit9     : Interrupt on TX                    */
            INTR,   /* bit10    : Interrupt on RX                    */
            I2CEN,  /* bit15    : BSC controller enable              */
            C_ALL
        };

        /// @brief Fields of the Status register (S). DONE/ERR/CLKT are W1C.
        enum Status : std::uint32_t {
            TA,     /* bit0  : Transfer Active            */
            DONE,   /* bit1  : Transfer Done (W1C)        */
            TXW,    /* bit2  : FIFO needs Writing         */
            RXR,    /* bit3  : FIFO needs Reading         */
            TXD,    /* bit4  : FIFO can accept data       */
            RXD,    /* bit5  : FIFO contains data         */
            TXE,    /* bit6  : FIFO empty                 */
            RXF,    /* bit7  : FIFO full                  */
            ERR,    /* bit8  : ACK error (W1C)            */
            CLKT_TO,/* bit9  : Clock stretch timeout (W1C)*/
            S_ALL
        };

        using device_register = mmio_reg;
        BSCRegistersAddress() {}
        ~BSCRegistersAddress() {}

        void *operator new(std::size_t nBytes, void *region=nullptr) {
            (void)nBytes;
            if(nullptr == region) {
                /* BSC1 physical base (bus 0x7E804000 -> phys 0x3F804000). */
                return reinterpret_cast<void *>(0x3F804000U);
            }
            return(region);
        }

        device_register m_register[Register::BSC_MAX];
    };

    /**
     * @brief
     *      System Timer register block. Refer Section 12 "System Timer" of
     *      BCM2837-ARM-Peripherals.pdf. The peripheral is at bus 0x7E003000 ->
     *      phys 0x3F003000 and exposes a free-running 1 MHz counter (so one
     *      tick == 1 microsecond) plus four 32-bit compare channels.
     *
     *      The counter (CLO/CHI) is read-only; writing a compare register Cn and
     *      enabling the matching System-Timer IRQ fires an interrupt when CLO
     *      reaches Cn (the match latches CS bit Mn, W1C). On the Raspberry Pi
     *      channels 0 and 2 are owned by the GPU/VideoCore firmware — only
     *      channels 1 and 3 are free for the ARM.
    */
    struct SystemTimerRegistersAddress {

        enum Register : std::uint32_t {
            CS,     /* 0x00 Control / Status (match flags M0..M3, W1C) */
            CLO,    /* 0x04 Counter Lower 32 bits (RO, 1 MHz)          */
            CHI,    /* 0x08 Counter Higher 32 bits (RO)                */
            C0,     /* 0x0C Compare 0 (GPU)                            */
            C1,     /* 0x10 Compare 1 (ARM-usable)                     */
            C2,     /* 0x14 Compare 2 (GPU)                            */
            C3,     /* 0x18 Compare 3 (ARM-usable)                     */
            ST_MAX
        };

        /// @brief Match bits in CS (write 1 to clear). Mn latches when CLO==Cn.
        enum Status : std::uint32_t {
            M0,     /* bit0 : compare-0 matched */
            M1,     /* bit1 : compare-1 matched */
            M2,     /* bit2 : compare-2 matched */
            M3,     /* bit3 : compare-3 matched */
            M_ALL
        };

        using device_register = mmio_reg;
        SystemTimerRegistersAddress() {}
        ~SystemTimerRegistersAddress() {}

        void *operator new(std::size_t nBytes, void *region=nullptr) {
            (void)nBytes;
            if(nullptr == region) {
                /* System Timer physical base (bus 0x7E003000 -> phys 0x3F003000). */
                return reinterpret_cast<void *>(0x3F003000U);
            }
            return(region);
        }

        device_register m_register[Register::ST_MAX];
    };

    /**
     * @brief
     *      SPI0 master register block. Refer Section 10 "SPI" of
     *      BCM2837-ARM-Peripherals.pdf. SPI0 is at bus 0x7E204000 ->
     *      phys 0x3F204000 and is exposed on GPIO7..11 (ALT0):
     *      GPIO8=CE0, GPIO7=CE1, GPIO9=MISO, GPIO10=MOSI, GPIO11=SCLK.
    */
    struct SPIRegistersAddress {

        enum Register : std::uint32_t {
            CS,     /* 0x00 Control and Status     */
            FIFO,   /* 0x04 TX/RX FIFO             */
            CLK,    /* 0x08 Clock divider          */
            DLEN,   /* 0x0C Data length (DMA)      */
            LTOH,   /* 0x10 LoSSI output hold delay*/
            DC,     /* 0x14 DMA DREQ controls      */
            SPI_MAX
        };

        /// @brief Fields of the CS (control/status) register.
        enum ControlStatus : std::uint32_t {
            CS_LINE,  /* bits[1:0] : Chip Select (which CE line)            */
            CPHA,     /* bit2      : Clock Phase                            */
            CPOL,     /* bit3      : Clock Polarity                         */
            CLEAR,    /* bits[5:4] : Clear TX (0b01) / RX (0b10) FIFO       */
            CSPOL,    /* bit6      : Chip Select Polarity                   */
            TA,       /* bit7      : Transfer Active                        */
            DMAEN,    /* bit8      : DMA enable                             */
            INTD,     /* bit9      : Interrupt on DONE                      */
            INTR,     /* bit10     : Interrupt on RXR                       */
            ADCS,     /* bit11     : Auto Deassert Chip Select (DMA)        */
            REN,      /* bit12     : Read Enable (bidirectional mode)       */
            LEN,      /* bit13     : LoSSI enable                           */
            LMONO,    /* bit14     : unused (LoSSI)                         */
            TE_EN,    /* bit15     : unused                                 */
            DONE,     /* bit16     : Transfer Done (RO)                     */
            RXD,      /* bit17     : RX FIFO contains data (RO)             */
            TXD,      /* bit18     : TX FIFO can accept data (RO)           */
            RXR,      /* bit19     : RX FIFO needs Reading (RO)             */
            RXF,      /* bit20     : RX FIFO full (RO)                      */
            CSPOL0,   /* bit21     : Chip Select 0 Polarity                 */
            CSPOL1,   /* bit22     : Chip Select 1 Polarity                 */
            CSPOL2,   /* bit23     : Chip Select 2 Polarity                 */
            DMA_LEN,  /* bit24     : Enable DMA in LoSSI mode               */
            LEN_LONG, /* bit25     : Enable Long data word in LoSSI DMA     */
            CS_ALL
        };

        using device_register = mmio_reg;
        SPIRegistersAddress() {}
        ~SPIRegistersAddress() {}

        void *operator new(std::size_t nBytes, void *region=nullptr) {
            (void)nBytes;
            if(nullptr == region) {
                /* SPI0 physical base (bus 0x7E204000 -> phys 0x3F204000). */
                return reinterpret_cast<void *>(0x3F204000U);
            }
            return(region);
        }

        device_register m_register[Register::SPI_MAX];
    };
}











#endif /*__memory_map_hpp__*/