#include "bytecode.hpp"
#include "interp.hpp"
#include "vcode.h"
#include "phase.h"

#include <gtest/gtest.h>

#define __ asm_.

using namespace testing;

class InterpTest : public ::testing::Test {
protected:
   InterpTest()
      : asm_(InterpMachine::get())
   {}

   virtual void SetUp() {}
   virtual void TearDown() {}

   Bytecode::Assembler asm_;
   Interpreter         interp_;
};

TEST_F(InterpTest, add1) {
   __ add(Bytecode::R(0), 1);
   __ ret();

   Bytecode *b = __ finish();

   interp_.set_reg(0, 5);
   EXPECT_EQ(6, interp_.run(b));

   interp_.set_reg(0, 42);
   EXPECT_EQ(43, interp_.run(b));
}

TEST_F(InterpTest, fact) {
   Bytecode::Register Rn = Bytecode::R(0);
   Bytecode::Register Rtmp1 = Bytecode::R(1);
   Bytecode::Register Rtmp2 = Bytecode::R(8);
   Bytecode::Register Rtmp3 = Bytecode::R(9);

   Bytecode::Label L1, L2, L3;

   __ mov(Rtmp1, 1);
   __ str(__ sp(), 0, Rtmp1);
   __ cmp(Rtmp1, Rn);
   __ jmp(L2, Bytecode::GT);
   __ jmp(L1);
   __ bind(L1);
   __ str(__ sp(), 4, Rtmp1);
   __ jmp(L2);
   __ bind(L3);
   __ ldr(Rn, __ sp(), 0);
   __ ret();
   __ bind(L2);
   __ ldr(Rtmp2, __ sp(), 0);
   __ ldr(Rtmp3, __ sp(), 4);
   __ mul(Rtmp2, Rtmp3);
   __ str(__ sp(), 0, Rtmp2);
   __ cmp(Rtmp3, Rn);
   __ add(Rtmp3, 1);
   __ str(__ sp(), 4, Rtmp3);
   __ jmp(L3, Bytecode::EQ);
   __ jmp(L2);

   Bytecode *b = __ finish();

   interp_.set_reg(0, 1);
   EXPECT_EQ(1, interp_.run(b));

   interp_.set_reg(0, 5);
   EXPECT_EQ(120, interp_.run(b));

   interp_.set_reg(0, 10);
   EXPECT_EQ(3628800, interp_.run(b));
}
