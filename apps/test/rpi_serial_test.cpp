/// PSK provisioning (task A) — RPi serial-number reader tests.

#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "rpi_serial.hpp"

namespace {

std::string make_tmpdir() {
    char tpl[64] = "/tmp/rpi-serial-XXXXXX";
    if (char* d = mkdtemp(tpl)) return d;
    ::mkdir("/tmp", 01777);
    std::strcpy(tpl, "/tmp/rpi-serial-XXXXXX");
    if (char* d = mkdtemp(tpl)) return d;
    std::strcpy(tpl, "./rpi-serial-XXXXXX");
    if (char* d = mkdtemp(tpl)) return d;
    return {};
}

void write_file(const std::string& path, const std::string& body) {
    std::ofstream(path, std::ios::binary).write(body.data(),
                                                static_cast<std::streamsize>(body.size()));
}

} // namespace

TEST(RpiSerial, ReadsSerialFromDeviceTree) {
    auto dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP();
    const std::string dt = dir + "/serial-number";
    // device-tree property is a NUL-terminated string.
    write_file(dt, std::string("100000003d1f9c2e\0", 17));

    EXPECT_EQ("100000003d1f9c2e",
              iot::read_rpi_serial(dt, dir + "/cpuinfo-absent"));
    ::unlink(dt.c_str()); ::rmdir(dir.c_str());
}

TEST(RpiSerial, FallsBackToCpuinfo) {
    auto dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP();
    const std::string cpu = dir + "/cpuinfo";
    write_file(cpu,
        "processor\t: 0\n"
        "model name\t: ARMv7\n"
        "Serial\t\t: 0000000012345678\n");

    // device-tree path does not exist → fall back to cpuinfo.
    EXPECT_EQ("0000000012345678",
              iot::read_rpi_serial(dir + "/serial-absent", cpu));
    ::unlink(cpu.c_str()); ::rmdir(dir.c_str());
}

TEST(RpiSerial, ReturnsEmptyWhenNeitherPresent) {
    auto dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP();
    EXPECT_EQ("", iot::read_rpi_serial(dir + "/nope1", dir + "/nope2"));
    ::rmdir(dir.c_str());
}

TEST(RpiSerial, DeviceTreePreferredOverCpuinfo) {
    auto dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP();
    const std::string dt = dir + "/serial-number";
    const std::string cpu = dir + "/cpuinfo";
    write_file(dt, std::string("AAAA1111\0", 9));
    write_file(cpu, "Serial\t\t: BBBB2222\n");
    EXPECT_EQ("AAAA1111", iot::read_rpi_serial(dt, cpu));
    ::unlink(dt.c_str()); ::unlink(cpu.c_str()); ::rmdir(dir.c_str());
}

TEST(RpiSerial, IsRpiDetectsModel) {
    auto dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP();
    const std::string model = dir + "/model";
    write_file(model, std::string("Raspberry Pi 4 Model B Rev 1.4\0", 31));
    EXPECT_TRUE(iot::is_rpi(model));
    ::unlink(model.c_str()); ::rmdir(dir.c_str());
}

TEST(RpiSerial, IsRpiFalseForOtherHardware) {
    auto dir = make_tmpdir();
    if (dir.empty()) GTEST_SKIP();
    const std::string model = dir + "/model";
    write_file(model, "Some Generic x86 Board\n");
    EXPECT_FALSE(iot::is_rpi(model));
    // Missing model file → not an RPi.
    EXPECT_FALSE(iot::is_rpi(dir + "/model-absent"));
    ::unlink(model.c_str()); ::rmdir(dir.c_str());
}
