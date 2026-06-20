#include "sensord.hpp"

#include <exception>
#include <vector>

#include <ace/Log_Msg.h>
#include <ace/OS_NS_unistd.h>
#include <ace/Time_Value.h>

#include "data_store/client.hpp"
#include "mmio.hpp"            // BCM2837::map_i2c / map_gpio / phys bases
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

} // namespace

int run(const Options& opt) {
    data_store::Client cli;
    if (!cli.connect(opt.ds_sock).ok) {
        ACE_ERROR_RETURN((LM_ERROR,
            ACE_TEXT("%D [sensord] data-store connect failed\n")), 1);
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
            ACE_TEXT("%D [sensord] bus up; sampling every %us\n"), opt.interval_sec));

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
    } catch (const std::exception& e) {
        ACE_ERROR_RETURN((LM_ERROR,
            ACE_TEXT("%D [sensord] I2C map/init failed: %C\n"), e.what()), 2);
    }
    return 0;
}

} // namespace sensors
