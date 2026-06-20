#ifndef __mmio_hpp__
#define __mmio_hpp__

/**
 * @brief
 *      Linux real-hardware backing for the bcm2837 drivers. Maps a BCM2837
 *      peripheral register block into the process through /dev/mem and hands the
 *      resulting virtual pointer to a driver's region constructor — the same
 *      seam the unit tests use, except the buffer is live MMIO instead of a
 *      std::vector.
 *
 *      This works because BCM2837::mmio_reg is trivially default-constructible:
 *      placement-new'ing the register block over the mapping does not run any
 *      element constructor, so the live registers are *overlaid*, not zeroed
 *      (see memory_map.hpp / docs/DRIVER_REVIEW.md §2.1).
 *
 *      /dev/mem requires CAP_SYS_RAWIO (typically root). For GPIO alone you can
 *      instead use the unprivileged /dev/gpiomem, which maps the GPIO block at
 *      offset 0 — pass dev="/dev/gpiomem" and phys=0 to MMIO, or call
 *      map_gpiomem().
 *
 *      POSIX/Linux only — this header pulls in <sys/mman.h> et al. The core
 *      drivers and the gtest suite do not include it, so the host/test build is
 *      unaffected.
*/

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#include "memory_map.hpp"
#include "gpio.hpp"
#include "clock.hpp"
#include "i2c.hpp"
#include "spi.hpp"
// interrupt.hpp is intentionally NOT mapped here — see the IRQ note below.

namespace BCM2837 {

    /// @brief BCM2837 (Pi 2/3) peripheral physical bases. Datasheet bus
    ///        addresses 0x7Ennnnnn map to physical 0x3Fnnnnnn.
    inline constexpr std::uintptr_t PERIPH_BASE = 0x3F000000U;
    inline constexpr std::uintptr_t GPIO_PHYS   = PERIPH_BASE + 0x200000U; ///< 0x3F200000
    inline constexpr std::uintptr_t CLOCK_PHYS  = PERIPH_BASE + 0x101070U; ///< 0x3F101070 (CM_GP0CTL)
    inline constexpr std::uintptr_t I2C1_PHYS   = PERIPH_BASE + 0x804000U; ///< 0x3F804000 (BSC1)
    inline constexpr std::uintptr_t SPI0_PHYS   = PERIPH_BASE + 0x204000U; ///< 0x3F204000

    /**
     * @brief RAII mapping of one peripheral register block.
     *
     * Maps the page(s) covering [phys, phys+length) and exposes a pointer to
     * the requested block via get(). The mapping is page-aligned internally, so
     * any phys offset (e.g. the clock block at 0x...1070) is handled.
    */
    class MMIO {
        public:
            MMIO(std::uintptr_t phys, std::size_t length, const char* dev = "/dev/mem") {
                m_fd = ::open(dev, O_RDWR | O_SYNC | O_CLOEXEC);
                if(m_fd < 0) {
                    throw std::runtime_error(std::string("bcm2837 MMIO: open(") + dev + ") failed");
                }
                const std::uintptr_t pg = static_cast<std::uintptr_t>(::sysconf(_SC_PAGESIZE));
                const std::uintptr_t page_base = phys & ~(pg - 1U);
                m_offset = static_cast<std::size_t>(phys - page_base);
                m_length = m_offset + length;
                m_map = ::mmap(nullptr, m_length, PROT_READ | PROT_WRITE, MAP_SHARED,
                               m_fd, static_cast<off_t>(page_base));
                if(m_map == MAP_FAILED) {
                    ::close(m_fd);
                    m_fd = -1;
                    throw std::runtime_error("bcm2837 MMIO: mmap failed");
                }
            }

            ~MMIO() {
                if(m_map && m_map != MAP_FAILED) ::munmap(m_map, m_length);
                if(m_fd >= 0) ::close(m_fd);
            }

            MMIO(const MMIO&) = delete;
            MMIO& operator=(const MMIO&) = delete;

            MMIO(MMIO&& o) noexcept { swap(o); }
            MMIO& operator=(MMIO&& o) noexcept { swap(o); return *this; }

            /// @brief Pointer to the mapped register block (page offset applied).
            void* get() const { return static_cast<char*>(m_map) + m_offset; }

        private:
            void swap(MMIO& o) noexcept {
                std::swap(m_fd, o.m_fd);
                std::swap(m_map, o.m_map);
                std::swap(m_offset, o.m_offset);
                std::swap(m_length, o.m_length);
            }

            int m_fd = -1;
            void* m_map = MAP_FAILED;
            std::size_t m_offset = 0;
            std::size_t m_length = 0;
    };

    /**
     * @brief A driver bound to a live MMIO mapping it owns.
     *
     * Member order matters: m_io maps first, then m_drv is constructed over
     * m_io.get(). Non-copyable; usable by value via guaranteed copy elision
     * (the map_* factories return a prvalue).
     *
     *   auto gpio = BCM2837::map_gpio();   // root via /dev/mem
     *   gpio->output(17);
     *   gpio->GPSETn(17);
    */
    template<class Driver>
    class Mapped {
        public:
            Mapped(std::uintptr_t phys, std::size_t length, const char* dev = "/dev/mem")
                : m_io(phys, length, dev), m_drv(m_io.get()) {}

            Driver*       operator->()       { return &m_drv; }
            const Driver* operator->() const { return &m_drv; }
            Driver&       operator*()        { return m_drv; }

            Mapped(const Mapped&) = delete;
            Mapped& operator=(const Mapped&) = delete;

        private:
            MMIO   m_io;
            Driver m_drv;
    };

    inline Mapped<GPIO>  map_gpio()  { return Mapped<GPIO>(GPIO_PHYS,  sizeof(GPIORegistersAddress)); }
    inline Mapped<CLOCK> map_clock() { return Mapped<CLOCK>(CLOCK_PHYS, sizeof(ClockRegistersAddress)); }
    inline Mapped<I2C>   map_i2c()   { return Mapped<I2C>(I2C1_PHYS,   sizeof(BSCRegistersAddress)); }
    inline Mapped<SPI>   map_spi()   { return Mapped<SPI>(SPI0_PHYS,   sizeof(SPIRegistersAddress)); }

    // No map_irq(): the IRQ driver pairs the interrupt-enable registers with an
    // IVT (ARM exception vector table) that its region ctor places *past* the
    // register block — neither the extra placement nor a userspace-managed
    // vector table fits the Linux /dev/mem model (interrupts are the kernel's
    // job). IRQ stays a bare-metal-only construct; use it only in a freestanding
    // build, not through this mapper.

    /// @brief Unprivileged GPIO-only mapping via /dev/gpiomem (group gpio, no
    ///        root). The driver exposes the GPIO block at offset 0 of the device.
    inline Mapped<GPIO> map_gpiomem() {
        return Mapped<GPIO>(0, sizeof(GPIORegistersAddress), "/dev/gpiomem");
    }

} // namespace BCM2837

#endif /* __mmio_hpp__ */
