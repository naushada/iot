#include "sensord.hpp"

#include <cerrno>
#include <exception>
#include <vector>

#include <ace/Log_Msg.h>
#include <ace/OS_NS_unistd.h>
#include <ace/Time_Value.h>

#include "data_store/client.hpp"
#include "i2c_dev.hpp"         // I2cDevTransport (preferred: /dev/i2c-N)
#include "mmio.hpp"            // BCM2837::map_i2c / map_gpio (BSC fallback)
#include "sensor_reader.hpp"

namespace sensors {

namespace {

/// Translate the cache's string KV batch into a ds set() batch. The version
/// key is integer-typed in the schema, so it is set as int32; everything else
/// is a formatted string.
std::vector<data_store::KV> to_ds_batch(const std::vector<KV>& kv) {
    std::vector<data_store::KV> batch;
    batch.reserve(kv.size());
    for (const auto& e : kv) {
        if (e.key == "iot.sensor.version") {
            std::int32_t v = 0;
            try { v = static_cast<std::int32_t>(std::stol(e.value)); } catch (...) {}
            batch.emplace_back(e.key, data_store::Value{v});
        } else {
            batch.emplace_back(e.key, data_store::Value{e.value});
        }
    }
    return batch;
}

/// Sample → publish loop over whichever transport we ended up with.
int run_loop(I2cTransport& bus, data_store::Client& cli, const Options& opt) {
    SensorCache cache;
    for (;;) {
        const SampleResult r = sample_all(bus, cache,
                                          opt.bme_addr, opt.imu_addr, opt.light_addr);
        if (r.any()) {
            const auto batch = to_ds_batch(cache.to_kv());
            if (!batch.empty() && !cli.set(batch).ok) {
                ACE_ERROR((LM_ERROR,
                    ACE_TEXT("%D [sensord] ds set(iot.sensor.*) failed\n")));
            }
        } else {
            ACE_DEBUG((LM_DEBUG,
                ACE_TEXT("%D [sensord] no sensors responded this tick\n")));
        }
        if (opt.once) {
            break;
        }
        ACE_OS::sleep(ACE_Time_Value(static_cast<time_t>(opt.interval_sec)));
    }
    return 0;
}

} // namespace

int run(const Options& opt) {
    data_store::Client cli;
    if (!cli.connect(opt.ds_sock).ok) {
        ACE_ERROR_RETURN((LM_ERROR,
            ACE_TEXT("%D [sensord] data-store connect failed\n")), 1);
    }

    // Preferred transport: the kernel i2c-dev node (/dev/i2c-N). It needs only
    // r/w on the device (group i2c) — no CAP_SYS_RAWIO / /dev/mem — and gives a
    // real repeated-START. Falls back to the BSC register transport below.
    if (!opt.i2c_dev.empty()) {
        I2cDevTransport dev;
        if (dev.open(opt.i2c_dev) == 0) {
            ACE_DEBUG((LM_INFO,
                ACE_TEXT("%D [sensord] using %C (i2c-dev); sampling every %us\n"),
                opt.i2c_dev.c_str(), opt.interval_sec));
            return run_loop(dev, cli, opt);
        }
        ACE_DEBUG((LM_WARNING,
            ACE_TEXT("%D [sensord] %C unavailable (errno=%d); trying BSC /dev/mem\n"),
            opt.i2c_dev.c_str(), errno));
    }

    try {
        // BSC1 + GPIO live registers. Throws if /dev/mem is unreadable (missing
        // CAP_SYS_RAWIO / not a Pi), caught below so we exit non-zero.
        BCM2837::Mapped<I2C>  i2cMap(BCM2837::I2C1_PHYS,
                                     sizeof(BCM2837::BSCRegistersAddress));
        BCM2837::Mapped<GPIO> gpioMap(BCM2837::GPIO_PHYS,
                                      sizeof(BCM2837::GPIORegistersAddress));
        Bcm2837I2cTransport bus(*i2cMap, *gpioMap);
        bus.bus_init();
        ACE_DEBUG((LM_INFO,
            ACE_TEXT("%D [sensord] using BSC /dev/mem; sampling every %us\n"),
            opt.interval_sec));
        return run_loop(bus, cli, opt);
    } catch (const std::exception& e) {
        ACE_ERROR_RETURN((LM_ERROR,
            ACE_TEXT("%D [sensord] no usable I2C transport: %C\n"), e.what()), 2);
    }
}

} // namespace sensors
