#ifndef __clock_test_cpp__
#define __clock_test_cpp__

#include "clock_test.hpp"

void CLOCKTest::SetUp() {
    
}

void CLOCKTest::TearDown() {

}

void CLOCKTest::TestBody() {

}

TEST_F(CLOCKTest, ClockRegister_CM_GP0DIV_Read_DIVI) {
    clock().write(RPi3B::ClockRegistersAddress::Register::CM_GP0DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVI, 35);
    auto res = clock().read(RPi3B::ClockRegistersAddress::Register::CM_GP0DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVI);
    EXPECT_EQ(res, 35);
}

TEST_F(CLOCKTest, ClockRegister_CM_GP1DIV_Read_DIVI) {
    clock().write(RPi3B::ClockRegistersAddress::Register::CM_GP1DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVI, 35);
    auto res = clock().read(RPi3B::ClockRegistersAddress::Register::CM_GP1DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVI);
    EXPECT_EQ(res, 35);
}

TEST_F(CLOCKTest, ClockRegister_CM_GP2DIV_Read_DIVI) {
    clock().write(RPi3B::ClockRegistersAddress::Register::CM_GP2DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVI, 35);
    auto res = clock().read(RPi3B::ClockRegistersAddress::Register::CM_GP2DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVI);
    EXPECT_EQ(res, 35);
}

TEST_F(CLOCKTest, ClockRegister_CM_GP0DIV_Read_DIVF) {
    clock().write(RPi3B::ClockRegistersAddress::Register::CM_GP0DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVF, 492);
    auto res = clock().read(RPi3B::ClockRegistersAddress::Register::CM_GP0DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVF);
    EXPECT_EQ(res, 492);
}

TEST_F(CLOCKTest, ClockRegister_CM_GP1DIV_Read_DIVF) {
    clock().write(RPi3B::ClockRegistersAddress::Register::CM_GP1DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVF, 492);
    auto res = clock().read(RPi3B::ClockRegistersAddress::Register::CM_GP1DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVF);
    EXPECT_EQ(res, 492);
}

TEST_F(CLOCKTest, ClockRegister_CM_GP2DIV_Read_DIVF) {
    clock().write(RPi3B::ClockRegistersAddress::Register::CM_GP2DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVF, 492);
    auto res = clock().read(RPi3B::ClockRegistersAddress::Register::CM_GP2DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVF);
    EXPECT_EQ(res, 492);
}

TEST_F(CLOCKTest, ClockRegister_CM_GP0DIV_Read_BOTH) {
    clock().write(RPi3B::ClockRegistersAddress::Register::CM_GP0DIV, RPi3B::ClockRegistersAddress::ClockDivisor::BOTH_VALUE, ((35 << 12) | 492));
    auto res = clock().read(RPi3B::ClockRegistersAddress::Register::CM_GP0DIV, RPi3B::ClockRegistersAddress::ClockDivisor::BOTH_VALUE);
    res = clock().read(RPi3B::ClockRegistersAddress::Register::CM_GP0DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVF);
    EXPECT_EQ(res, 492);
    res = clock().read(RPi3B::ClockRegistersAddress::Register::CM_GP0DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVI);
    EXPECT_EQ(res, 35);
}

TEST_F(CLOCKTest, ClockRegister_CM_GP1DIV_Read_BOTH) {
    clock().write(RPi3B::ClockRegistersAddress::Register::CM_GP1DIV, RPi3B::ClockRegistersAddress::ClockDivisor::BOTH_VALUE, ((35 << 12) | 492));
    auto res = clock().read(RPi3B::ClockRegistersAddress::Register::CM_GP1DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVF);
    EXPECT_EQ(res, 492);
    res = clock().read(RPi3B::ClockRegistersAddress::Register::CM_GP1DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVI);
    EXPECT_EQ(res, 35);
}

TEST_F(CLOCKTest, ClockRegister_CM_GP2DIV_Read_BOTH) {
    clock().write(RPi3B::ClockRegistersAddress::Register::CM_GP2DIV, RPi3B::ClockRegistersAddress::ClockDivisor::BOTH_VALUE, ((35 << 12) | 492));
    auto res = clock().read(RPi3B::ClockRegistersAddress::Register::CM_GP2DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVF);
    EXPECT_EQ(res, 492);
    res = clock().read(RPi3B::ClockRegistersAddress::Register::CM_GP2DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVI);
    EXPECT_EQ(res, 35);
}

TEST_F(CLOCKTest, ClockRegister_CM_GP0DIV_Write_BOTH) {
    clock().write(RPi3B::ClockRegistersAddress::Register::CM_GP0DIV, RPi3B::ClockRegistersAddress::ClockDivisor::BOTH_VALUE, ((0xFFF << 12) | 0xFFF));
    auto res = clock().read(RPi3B::ClockRegistersAddress::Register::CM_GP0DIV, RPi3B::ClockRegistersAddress::ClockDivisor::BOTH_VALUE);
    res = clock().read(RPi3B::ClockRegistersAddress::Register::CM_GP0DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVF);
    EXPECT_EQ(res, 0xFFF);
    res = clock().read(RPi3B::ClockRegistersAddress::Register::CM_GP0DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVI);
    EXPECT_EQ(res, 0xFFF);
}

TEST_F(CLOCKTest, ClockRegister_CM_GP1DIV_Write_BOTH) {
    clock().write(RPi3B::ClockRegistersAddress::Register::CM_GP1DIV, RPi3B::ClockRegistersAddress::ClockDivisor::BOTH_VALUE, ((0xFF2 << 12) | 0xFFD));
    auto res = clock().read(RPi3B::ClockRegistersAddress::Register::CM_GP1DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVF);
    EXPECT_EQ(res, 0xFFD);
    res = clock().read(RPi3B::ClockRegistersAddress::Register::CM_GP1DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVI);
    EXPECT_EQ(res, 0xFF2);
}

TEST_F(CLOCKTest, ClockRegister_CM_GP2DIV_Write_BOTH) {
    clock().write(RPi3B::ClockRegistersAddress::Register::CM_GP2DIV, RPi3B::ClockRegistersAddress::ClockDivisor::BOTH_VALUE, ((0x1 << 12) | 0x1));
    auto res = clock().read(RPi3B::ClockRegistersAddress::Register::CM_GP2DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVF);
    EXPECT_EQ(res, 0x1);
    res = clock().read(RPi3B::ClockRegistersAddress::Register::CM_GP2DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVI);
    EXPECT_EQ(res, 0x1);
}

TEST_F(CLOCKTest, ClockRegister_CM_GP2DIV_Write_BOTH_Var1) {
    clock().write(RPi3B::ClockRegistersAddress::Register::CM_GP2DIV, RPi3B::ClockRegistersAddress::ClockDivisor::BOTH_VALUE, ((21 << 12) | 854));
    auto res = clock().read(RPi3B::ClockRegistersAddress::Register::CM_GP2DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVF);
    EXPECT_EQ(res, 854);
    res = clock().read(RPi3B::ClockRegistersAddress::Register::CM_GP2DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVI);
    EXPECT_EQ(res, 21);
}

TEST_F(CLOCKTest, ClockRegister_CM_GP2DIV_Write_BOTH_Var2) {
    clock().write(RPi3B::ClockRegistersAddress::Register::CM_GP2DIV, RPi3B::ClockRegistersAddress::ClockDivisor::BOTH_VALUE, ((10 << 12) | 939));
    auto res = clock().read(RPi3B::ClockRegistersAddress::Register::CM_GP2DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVF);
    EXPECT_EQ(res, 939);
    res = clock().read(RPi3B::ClockRegistersAddress::Register::CM_GP2DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVI);
    EXPECT_EQ(res, 10);
}

TEST_F(CLOCKTest, ClockRegister_CM_GP2DIV_clr_CM_GPnDIV_n0) {
    clock().set_CM_GPnDIV(RPi3B::ClockRegistersAddress::CM_GP0DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVI, 429);
    auto value = clock().get_CM_GPnDIV(RPi3B::ClockRegistersAddress::CM_GP0DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVI);
    EXPECT_EQ(value, 429);
    clock().clr_CM_GPnDIV(RPi3B::ClockRegistersAddress::CM_GP0DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVI);
    value = clock().get_CM_GPnDIV(RPi3B::ClockRegistersAddress::CM_GP0DIV, RPi3B::ClockRegistersAddress::ClockDivisor::DIVI);
    EXPECT_EQ(value, 0);
    
}

TEST_F(CLOCKTest, ClockRegister_CM_GP0CTL_clr_CM_GP0CTL_SRC) {
    clock().set_CM_GPnCTL(RPi3B::ClockRegistersAddress::CM_GP0CTL, RPi3B::ClockRegistersAddress::SRC, 5);
    auto value = clock().get_CM_GPnCTL(RPi3B::ClockRegistersAddress::CM_GP0CTL, RPi3B::ClockRegistersAddress::SRC);
    EXPECT_EQ(value, 5);
    clock().clr_CM_GPnCTL(RPi3B::ClockRegistersAddress::CM_GP0CTL, RPi3B::ClockRegistersAddress::SRC);
    value = clock().get_CM_GPnCTL(RPi3B::ClockRegistersAddress::CM_GP0CTL, RPi3B::ClockRegistersAddress::SRC);
    EXPECT_EQ(value, 0);
    
}

TEST_F(CLOCKTest, ClockRegister_CM_GP0CTL_clr_CM_GP0CTL_MASH) {
    clock().set_CM_GPnCTL(RPi3B::ClockRegistersAddress::CM_GP0CTL, RPi3B::ClockRegistersAddress::SRC, 6);
    auto value = clock().get_CM_GPnCTL(RPi3B::ClockRegistersAddress::CM_GP0CTL, RPi3B::ClockRegistersAddress::SRC);
    EXPECT_EQ(value, 6);
    clock().set_CM_GPnCTL(RPi3B::ClockRegistersAddress::CM_GP0CTL, RPi3B::ClockRegistersAddress::MASH, 1);
    value = clock().get_CM_GPnCTL(RPi3B::ClockRegistersAddress::CM_GP0CTL, RPi3B::ClockRegistersAddress::ALL);
    ::printf("SRC+MASH 0x%X\n", value);
    value = clock().get_CM_GPnCTL(RPi3B::ClockRegistersAddress::CM_GP0CTL, RPi3B::ClockRegistersAddress::MASH);
    EXPECT_EQ(value, 1);
    clock().clr_CM_GPnCTL(RPi3B::ClockRegistersAddress::CM_GP0CTL, RPi3B::ClockRegistersAddress::MASH);
    value = clock().get_CM_GPnCTL(RPi3B::ClockRegistersAddress::CM_GP0CTL, RPi3B::ClockRegistersAddress::MASH);
    ::printf("0x%X\n", value);
    EXPECT_EQ(value, 0);
    
}

#endif /*__clock_test_cpp__*/