#ifndef __main_cpp__
#define __main_cpp__

#include <cstdio>
#include <cstring>
#include <exception>

#include "mmio.hpp"

/**
 * @brief Demo entry point. By default it only prints usage so the binary is
 *        safe to run on any host. With `--blink` it maps the GPIO block through
 *        /dev/mem and drives GPIO17 high — run that only on a Pi, as root (or
 *        switch map_gpio() for map_gpiomem() and join the `gpio` group).
*/
std::int32_t main(int argc, char** argv) {

    if(argc > 1 && std::strcmp(argv[1], "--blink") == 0) {
        try {
            auto gpio = BCM2837::map_gpio();          // root, /dev/mem
            const GPIO::gpio_number led = 17;
            gpio->output(led);
            gpio->GPSETn(led);                      // drive high
            std::printf("GPIO%u configured as output and driven high.\n", led);
        } catch(const std::exception& e) {
            std::fprintf(stderr, "MMIO mapping failed: %s\n", e.what());
            return(1);
        }
        return(0);
    }

    std::printf(
        "bcm2837 driver demo\n"
        "  %s --blink   map GPIO via /dev/mem and drive GPIO17 high (Pi, root)\n"
        "  See inc/mmio.hpp / README.md for the full real-MMIO API.\n",
        argv[0]);
    return(0);
}

#endif /*__main_cpp__*/
