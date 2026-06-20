#include "sensor_reader.hpp"

#include <cstdio>

namespace sensors {

namespace {
    std::string fmt(double v, int decimals) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
        return buf;
    }
    std::string triple(std::int16_t a, std::int16_t b, std::int16_t c) {
        return std::to_string(a) + "," + std::to_string(b) + "," + std::to_string(c);
    }
}

void SensorCache::set_env(double t, double p, double h) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_tempC = t; m_pressPa = p; m_humPct = h; m_haveEnv = true; ++m_version;
}

void SensorCache::set_lux(double lux) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_lux = lux; m_haveLux = true; ++m_version;
}

void SensorCache::set_imu(std::int16_t ax, std::int16_t ay, std::int16_t az,
                          std::int16_t gx, std::int16_t gy, std::int16_t gz) {
    std::lock_guard<std::mutex> lk(m_mtx);
    m_ax = ax; m_ay = ay; m_az = az; m_gx = gx; m_gy = gy; m_gz = gz;
    m_haveImu = true; ++m_version;
}

std::vector<KV> SensorCache::to_kv() const {
    std::lock_guard<std::mutex> lk(m_mtx);
    std::vector<KV> kv;
    if (m_haveEnv) {
        kv.push_back({"iot.sensor.temp",     fmt(m_tempC, 2)});
        kv.push_back({"iot.sensor.humidity", fmt(m_humPct, 2)});
        kv.push_back({"iot.sensor.pressure", fmt(m_pressPa, 1)});
    }
    if (m_haveLux) {
        kv.push_back({"iot.sensor.lux", fmt(m_lux, 2)});
    }
    if (m_haveImu) {
        kv.push_back({"iot.sensor.accel", triple(m_ax, m_ay, m_az)});
        kv.push_back({"iot.sensor.gyro",  triple(m_gx, m_gy, m_gz)});
    }
    if (!kv.empty()) {
        kv.push_back({"iot.sensor.version", std::to_string(m_version)});
    }
    return kv;
}

SampleResult sample_all(I2cTransport& bus, SensorCache& cache,
                        std::uint8_t bmeAddr, std::uint8_t imuAddr,
                        std::uint8_t lightAddr) {
    SampleResult res;

    Bme680 env(bus, bmeAddr);
    if (env.probe() && env.init()) {
        Bme680::Sample s{};
        if (env.read(s)) {
            cache.set_env(s.temperature_c, s.pressure_pa, s.humidity_pct);
            res.bme = true;
        }
    }

    Bmi160 imu(bus, imuAddr);
    if (imu.probe() && imu.init() == I2cResult::Ok) {
        Bmi160::Sample s{};
        if (imu.read(s)) {
            cache.set_imu(s.ax, s.ay, s.az, s.gx, s.gy, s.gz);
            res.imu = true;
        }
    }

    Opt3001 light(bus, lightAddr);
    if (light.probe() && light.init() == I2cResult::Ok) {
        double lux = 0.0;
        if (light.read_lux(lux)) {
            cache.set_lux(lux);
            res.light = true;
        }
    }

    return res;
}

} // namespace sensors
