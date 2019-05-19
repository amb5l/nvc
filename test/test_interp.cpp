#include "bytecode.hpp"
#include "interp.hpp"
#include "vcode.h"
#include "phase.h"
#include "test_util.h"

#define __ _asm.

START_TEST(test_add1)
{
   Bytecode::Assembler _asm(InterpMachine::get());
   Interpreter interp;

   __ add(Bytecode::R(0), 1);
   __ ret();

   Bytecode *b = __ finish();

   interp.set_reg(0, 5);
   fail_unless(6 == interp.run(b));

   interp.set_reg(0, 42);
   fail_unless(43 == interp.run(b));
}
END_TEST

START_TEST(test_fact)
{
   Bytecode::Assembler _asm(InterpMachine::get());
   Interpreter interp;

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

   interp.set_reg(0, 1);
   fail_unless(1 == interp.run(b));

   interp.set_reg(0, 5);
   fail_unless(120 == interp.run(b));

   interp.set_reg(0, 10);
   fail_unless(3628800 == interp.run(b));
}
END_TEST

START_TEST(test_uarray_len)
{
   vcode_unit_t unit = vcode_find_unit(
      ident_new("BC.FUNCTIONS.LEN(22BC.FUNCTIONS.INT_ARRAY)I"));
   fail_if(unit == nullptr);

   Interpreter interp;
   Bytecode *b = compile(InterpMachine::get(), unit);

   interp.push(10);        // Right
   interp.push(2);         // Left
   interp.push(RANGE_TO);  // Direction
   interp.push(0);         // Data pointer

   ck_assert_int_eq(9, interp.run(b));
}
END_TEST

START_TEST(test_uarray_get)
{
   vcode_unit_t unit = vcode_find_unit(
      ident_new("BC.FUNCTIONS.GET(22BC.FUNCTIONS.INT_ARRAYN)I"));
   fail_if(unit == nullptr);

   Interpreter interp;
   Bytecode *b = compile(InterpMachine::get(), unit);

   int datap = Interpreter::STACK_SIZE;
   interp.set_reg(0, datap);
   for (int i = 0; i < 10; i++)
      interp.mem_wr(0, 4 * i) = i;

   interp.push(10);        // Right
   interp.push(2);         // Left
   interp.push(RANGE_TO);  // Direction
   interp.push(datap);     // Data pointer

   for (int i = 0; i < 10; i++) {
      interp.set_reg(0, 2 + i);
      ck_assert_int_eq(i, interp.run(b));
   }
}
END_TEST

START_TEST(test_uarray_sum)
{
   vcode_unit_t unit = vcode_find_unit(
      ident_new("BC.FUNCTIONS.SUM(22BC.FUNCTIONS.INT_ARRAY)I"));
   fail_if(unit == nullptr);

   Interpreter interp;
   Bytecode *b = compile(InterpMachine::get(), unit);

   int datap = Interpreter::STACK_SIZE;
   interp.set_reg(0, datap);
   for (int i = 0; i < 10; i++)
      interp.mem_wr(0, 4 * i) = 1;

   interp.push(0);         // Right
   interp.push(0);         // Left
   interp.push(RANGE_TO);  // Direction
   interp.push(datap);     // Data pointer

   ck_assert_int_eq(9, interp.run(b));
}
END_TEST

extern "C" Suite *get_interp_tests(void)
{
   Suite *s = suite_create("interp");

   TCase *tc_basic = nvc_unit_test("basic");
   tcase_add_test(tc_basic, test_fact);
   tcase_add_test(tc_basic, test_add1);
   suite_add_tcase(s, tc_basic);

   TCase *tc_vhdl = nvc_unit_test("vhdl");
   nvc_add_bytecode_fixture(tc_vhdl);
   tcase_add_test(tc_vhdl, test_uarray_len);
   tcase_add_test(tc_vhdl, test_uarray_get);
   //tcase_add_test(tc_vhdl, test_uarray_sum);
   suite_add_tcase(s, tc_vhdl);

   return s;
}
