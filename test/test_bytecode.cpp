#include "bytecode.hpp"
#include "vcode.h"
#include "phase.h"
#include "test_util.h"

#include <map>

#define __ _asm.

namespace {
   struct CheckBytecode {
      CheckBytecode(uint16_t v) : value(v) {}

      static const uint16_t DONT_CARE = 0xffff;
      static const uint16_t REG_MASK  = 0x0100;

      uint16_t value;
   };
}

static const CheckBytecode _(CheckBytecode::DONT_CARE);
static const CheckBytecode _1(CheckBytecode::REG_MASK | 1);
static const CheckBytecode _2(CheckBytecode::REG_MASK | 2);
static const CheckBytecode _3(CheckBytecode::REG_MASK | 3);
static const CheckBytecode _4(CheckBytecode::REG_MASK | 4);

static vcode_unit_t context = nullptr;
static vcode_type_t i32_type = VCODE_INVALID_TYPE;

static void setup()
{
   context = emit_context(ident_new("unit_test"));
   i32_type = vtype_int(INT32_MIN, INT32_MAX);
}

static void teardown()
{
   vcode_unit_unref(context);
   context = nullptr;
   i32_type = VCODE_INVALID_TYPE;
}

static void check_bytecodes(const Bytecode *b,
                            const std::vector<CheckBytecode>&& expect)
{
   const uint8_t *p = b->code();
   std::map<int, int> match;

   for (const CheckBytecode& c : expect) {
      if (p >= b->code() + b->code_length()) {
         fail("expected more than %d bytecodes", b->code_length());
         return;
      }
      else if ((c.value & 0xff00) == 0) {
         // Directly compare the bytecode
         if (c.value != *p) {
            BufferPrinter printer;
            b->dump(printer, p - b->code());
            fail("bytecode mismatch at offset %d\n\n%s",
                 p - b->code(), printer.buffer());
         }
         else if (c.value != *p)
            return;
         ++p;
      }
      else if (c.value == CheckBytecode::DONT_CARE)
         ++p;
      else if ((c.value & CheckBytecode::REG_MASK) == CheckBytecode::REG_MASK) {
         const int num = c.value & 0xff;
         if (match.find(num) == match.end())
            match[num] = *p;
         else {
            if (match[num] != *p) {
               BufferPrinter printer;
               b->dump(printer);
               fail("placeholder _%d mismatch at offset %d\n\n%s",
                    num, p - b->code(), printer.buffer());
            }
            else if (match[num] != *p)
               return;
         }
         ++p;
      }
      else {
         fail("unexpected bytecode check %x", c.value);
         return;
      }
   }

   if ((int)b->code_length() != p - b->code()) {
      BufferPrinter printer;
      b->dump(printer);
      fail("did not match all bytecodes\n\n%s", printer.buffer());
   }
}

START_TEST(test_compile_add1)
{
   vcode_unit_t unit = emit_function(ident_new("add1"), context, i32_type);

   vcode_reg_t p0 = emit_param(i32_type, i32_type, ident_new("x"));
   emit_return(emit_add(p0, emit_const(i32_type, 1)));

   vcode_opt();

   Bytecode *b = compile(InterpMachine::get(), unit);
   fail_if(nullptr == b);

   check_bytecodes(b, {
         Bytecode::ENTER, 4, 0,
         Bytecode::ADDB, 0, 0x01,
         Bytecode::LEAVE,
         Bytecode::RET
      });

   vcode_unit_unref(unit);
   delete b;
}
END_TEST

START_TEST(test_select) {
   vcode_unit_t unit = emit_function(ident_new("max"), context, i32_type);

   vcode_reg_t p0 = emit_param(i32_type, i32_type, ident_new("x"));
   vcode_reg_t p1 = emit_param(i32_type, i32_type, ident_new("y"));
   vcode_reg_t cmp = emit_cmp(VCODE_CMP_GT, p0, p1);
   emit_return(emit_select(cmp, p0, p1));

   vcode_opt();

   Bytecode *b = compile(InterpMachine::get(), unit);
   fail_if(nullptr == b);

   check_bytecodes(b, {
         Bytecode::ENTER, 4, 0,
         Bytecode::CMP, _1, _2,
         Bytecode::MOV, _, _1,
         Bytecode::JMPC, Bytecode::GT, _, _,
         Bytecode::MOV, _, _2,
         Bytecode::MOV, 0, _,
         Bytecode::LEAVE,
         Bytecode::RET
      });

   vcode_unit_unref(unit);
   delete b;
}
END_TEST

START_TEST(test_patch)
{
   Bytecode::Assembler _asm(InterpMachine::get());
   Bytecode::Label L1;

   __ bind(L1);
   __ jmp(L1);
   __ jmp(L1);
   __ jmp(L1, Bytecode::LT);
   __ jmp(L1, Bytecode::GT);

   __ patch_branch(3, 0);
   __ patch_branch(6, 10);

   Bytecode *b = __ finish();

   check_bytecodes(b, {
         Bytecode::JMP, 0xff, 0xff,
         Bytecode::JMP, 0xfc, 0xff,
         Bytecode::JMPC, Bytecode::LT, 0x02, 0x00,
         Bytecode::JMPC, Bytecode::GT, 0xf4, 0xff,
      });

   delete b;
}
END_TEST

START_TEST(test_compile_fact)
{
   vcode_unit_t unit = vcode_find_unit(ident_new("BC.FUNCTIONS.FACT(I)I"));
   fail_if(nullptr == unit);

   vcode_select_unit(unit);

   Bytecode *b = compile(InterpMachine::get(), unit);
   fail_if(nullptr == b);

   check_bytecodes(b, {
         Bytecode::ENTER, 28, 0,
         Bytecode::MOVB, _, 1,
         Bytecode::STR, _, _, _, _,
         Bytecode::CMP, _, _,
         Bytecode::STR, _, _, _, _,
         Bytecode::JMPC, Bytecode::GT, _, _,
         Bytecode::JMP, _, _,
         Bytecode::MOVB, _, 1,
         Bytecode::STR, _, _, _, _,
         Bytecode::JMP, _, _,
         Bytecode::LDR, _, _, _, _,
         Bytecode::LEAVE,
         Bytecode::RET,
         Bytecode::LDR, _, _, _, _,
         Bytecode::LDR, _, _, _, _,
         Bytecode::MUL, _, _,
         Bytecode::STR, _, _, _, _,
         Bytecode::MOV, _, _,
         Bytecode::ADDB, _, 1,
         Bytecode::STR, _, _, _, _,
         Bytecode::LDR, _, _, _, _,
         Bytecode::CMP, _, _,
         Bytecode::JMPC, Bytecode::Z, _, _,
         Bytecode::JMP, _, _
      });

   delete b;
}
END_TEST

START_TEST(test_add_sub_reuse)
{
   vcode_unit_t unit = emit_function(ident_new("add_sub_reuse"),
                                     context, i32_type);

   vcode_reg_t p0 = emit_param(i32_type, i32_type, ident_new("x"));
   vcode_reg_t p1 = emit_param(i32_type, i32_type, ident_new("x"));

   vcode_reg_t t0 = emit_add(p0, p1);
   vcode_reg_t t1 = emit_sub(t0, p1);
   vcode_reg_t t2 = emit_add(t1, p0);
   emit_return(t2);

   vcode_opt();

   Bytecode *b = compile(InterpMachine::get(), unit);
   fail_if(nullptr == b);

   check_bytecodes(b, {
         Bytecode::ENTER, 12, 0,
         Bytecode::MOV, _1, 0,
         Bytecode::ADD, _1, 1,
         Bytecode::SUB, _1, 1,
         Bytecode::ADD, _1, 0,
         Bytecode::MOV, 0, _1,
         Bytecode::LEAVE,
         Bytecode::RET
      });

   vcode_unit_unref(unit);
   delete b;
}
END_TEST

START_TEST(test_unwrap)
{
   vcode_type_t pi32_type = vtype_pointer(i32_type);
   vcode_unit_t unit = emit_function(ident_new("unwrap"),
                                     context, pi32_type);

   vcode_type_t ui32_type = vtype_uarray(1, i32_type, i32_type);
   vcode_reg_t p0 = emit_param(ui32_type, ui32_type, ident_new("p0"));

   emit_return(emit_unwrap(p0));

   vcode_opt();

   Bytecode *b = compile(InterpMachine::get(), unit);
   fail_if(nullptr == b);

   check_bytecodes(b, {
         Bytecode::ENTER, 4, 0,
         Bytecode::LDR, 0, InterpMachine::FP_REG, 4, 0,
         Bytecode::LEAVE,
         Bytecode::RET
      });

   vcode_unit_unref(unit);
   delete b;
}
END_TEST

START_TEST(test_uarray_dir)
{
   vcode_unit_t unit = emit_function(ident_new("uarray_dir"),
                                     context, i32_type);

   vcode_type_t ui32_type = vtype_uarray(1, i32_type, i32_type);
   vcode_reg_t p0 = emit_param(ui32_type, ui32_type, ident_new("p0"));

   emit_return(emit_cast(i32_type, i32_type, emit_uarray_dir(p0, 0)));

   vcode_opt();

   Bytecode *b = compile(InterpMachine::get(), unit);
   fail_if(nullptr == b);

   check_bytecodes(b, {
         Bytecode::ENTER, 8, 0,
         Bytecode::LDR, 0, InterpMachine::FP_REG, 8, 0,
         Bytecode::LEAVE,
         Bytecode::RET
      });

   vcode_unit_unref(unit);
   delete b;
}
END_TEST

START_TEST(test_uarray_dir_highdim)
{
   vcode_unit_t unit = emit_function(ident_new("uarray_dir"),
                                     context, i32_type);

   vcode_type_t ui32_type = vtype_uarray(10, i32_type, i32_type);
   vcode_reg_t p0 = emit_param(ui32_type, ui32_type, ident_new("p0"));

   emit_return(emit_cast(i32_type, i32_type, emit_uarray_dir(p0, 9)));

   vcode_opt();

   Bytecode *b = compile(InterpMachine::get(), unit);
   fail_if(nullptr == b);

   check_bytecodes(b, {
         Bytecode::ENTER, 8, 0,
         Bytecode::LDR, 0, InterpMachine::FP_REG, 8, 0,
         Bytecode::TESTW, 0, 0, 2, 0, 0,
         Bytecode::CSET, 0, Bytecode::NZ,
         Bytecode::LEAVE,
         Bytecode::RET
      });

   vcode_unit_unref(unit);
   delete b;
}
END_TEST

START_TEST(test_uarray_left_right)
{
   vcode_unit_t unit = emit_function(ident_new("uarray_left_right"),
                                     context, i32_type);

   vcode_type_t ui32_type = vtype_uarray(1, i32_type, i32_type);
   vcode_reg_t p0 = emit_param(ui32_type, ui32_type, ident_new("p0"));

   vcode_reg_t left = emit_uarray_left(p0, 0);
   vcode_reg_t right = emit_uarray_right(p0, 0);
   emit_return(emit_cast(i32_type, i32_type, emit_add(left, right)));

   vcode_opt();

   Bytecode *b = compile(InterpMachine::get(), unit);
   fail_if(nullptr == b);

   check_bytecodes(b, {
         Bytecode::ENTER, 16, 0,
         Bytecode::LDR, _, InterpMachine::FP_REG, 12, 0,
         Bytecode::LDR, _, InterpMachine::FP_REG, 16, 0,
         Bytecode::ADD, 0, _,
         Bytecode::LEAVE,
         Bytecode::RET
      });

   vcode_unit_unref(unit);
   delete b;
}
END_TEST

START_TEST(test_range_null)
{
   vcode_unit_t unit = emit_function(ident_new("range_null"),
                                     context, vtype_bool());

   vcode_type_t ui32_type = vtype_uarray(1, i32_type, i32_type);
   vcode_reg_t p0 = emit_param(ui32_type, ui32_type, ident_new("p0"));

   vcode_reg_t left = emit_uarray_left(p0, 0);
   vcode_reg_t right = emit_uarray_right(p0, 0);
   vcode_reg_t dir = emit_uarray_dir(p0, 0);
   emit_return(emit_range_null(left, right, dir));

   vcode_opt();

   Bytecode *b = compile(InterpMachine::get(), unit);
   fail_if(nullptr == b);

   check_bytecodes(b, {
         Bytecode::ENTER, 16, 00,
         Bytecode::LDR, _, InterpMachine::FP_REG, 12, 0,
         Bytecode::LDR, _, InterpMachine::FP_REG, 16, 0,
         Bytecode::LDR, _1, InterpMachine::FP_REG, 8, 0,
         Bytecode::TESTB, _1, 1,
         Bytecode::JMPC, Bytecode::Z, _, _,
         Bytecode::CMP, _, _,
         Bytecode::CSET, _, Bytecode::GT,
         Bytecode::JMP, _, _,
         Bytecode::CMP, _, _,
         Bytecode::CSET, _, Bytecode::LT,
         Bytecode::MOV, 0, _,
         Bytecode::LEAVE,
         Bytecode::RET
      });

   vcode_unit_unref(unit);
   delete b;
}
END_TEST

START_TEST(test_uarray_deref)
{
   vcode_unit_t unit = emit_function(ident_new("uarray_deref"),
                                     context, i32_type);

   vcode_type_t ui32_type = vtype_uarray(1, i32_type, i32_type);
   vcode_reg_t p0 = emit_param(ui32_type, ui32_type, ident_new("p0"));

   vcode_reg_t data = emit_unwrap(p0);
   vcode_reg_t elem1 = emit_add(data, emit_const(vtype_offset(), 1));
   emit_return(emit_load_indirect(elem1));

   vcode_opt();

   Bytecode *b = compile(InterpMachine::get(), unit);
   fail_if(nullptr == b);

   check_bytecodes(b, {
         Bytecode::ENTER, 12, 0,
         Bytecode::LDR, 0, InterpMachine::FP_REG, 4, 0,
         Bytecode::ADDB, 0, 4,
         Bytecode::LDR, 0, 0, 0, 0,
         Bytecode::LEAVE,
         Bytecode::RET
      });

   vcode_unit_unref(unit);
   delete b;
}
END_TEST

START_TEST(test_uarray_deref2)
{
   vcode_unit_t unit = emit_function(ident_new("uarray_deref2"),
                                     context, i32_type);

   vcode_type_t ui32_type = vtype_uarray(1, i32_type, i32_type);
   vcode_reg_t p0 = emit_param(ui32_type, ui32_type, ident_new("p0"));
   vcode_reg_t p1 = emit_param(i32_type, i32_type, ident_new("p1"));

   vcode_type_t offset = vtype_offset();
   vcode_reg_t data = emit_unwrap(p0);
   vcode_reg_t elem1 = emit_add(data, emit_cast(offset, offset, p1));
   emit_return(emit_load_indirect(elem1));

   vcode_opt();

   Bytecode *b = compile(InterpMachine::get(), unit);
   fail_if(nullptr == b);

   check_bytecodes(b, {
         Bytecode::ENTER, 16, 0,
         Bytecode::LDR, 1, InterpMachine::FP_REG, 4, 0,
         Bytecode::MULB, 0, 4,
         Bytecode::ADD, 0, 1,
         Bytecode::LDR, 0, 0, 0, 0,
         Bytecode::LEAVE,
         Bytecode::RET
      });

   vcode_unit_unref(unit);
   delete b;
}
END_TEST

START_TEST(test_compile_hello)
{
   vcode_unit_t unit = vcode_find_unit(ident_new("BC.FUNCTIONS.HELLO"));
   fail_if(nullptr == unit);

   vcode_select_unit(unit);

   Bytecode *b = compile(InterpMachine::get(), unit);
   fail_if(nullptr == b);

   check_bytecodes(b, {
         Bytecode::ENTER, 4, 0,
         Bytecode::RELDATA, _, 0, 0,
         Bytecode::STR, _, _, _, _,   // Pointless
         Bytecode::MOVB, 0, SEVERITY_NOTE,
         Bytecode::LDR, 1, _, _, _,   // Unspilling store above
         Bytecode::MOVB, 2, 13,
         Bytecode::RTCALL, Bytecode::RT_REPORT,
         Bytecode::LEAVE,
         Bytecode::RET,
      });

   delete b;
}
END_TEST

extern "C" Suite *get_bytecode_tests(void)
{
   Suite *s = suite_create("bytecode");

   TCase *tc_vcode = nvc_unit_test("vcode");
   tcase_add_checked_fixture(tc_vcode, setup, teardown);
   tcase_add_test(tc_vcode, test_compile_add1);
   tcase_add_test(tc_vcode, test_patch);
   tcase_add_test(tc_vcode, test_select);
   tcase_add_test(tc_vcode, test_unwrap);
   tcase_add_test(tc_vcode, test_uarray_dir);
   tcase_add_test(tc_vcode, test_uarray_dir_highdim);
   tcase_add_test(tc_vcode, test_uarray_left_right);
   tcase_add_test(tc_vcode, test_add_sub_reuse);
   tcase_add_test(tc_vcode, test_range_null);
   tcase_add_test(tc_vcode, test_uarray_deref);
   tcase_add_test(tc_vcode, test_uarray_deref2);
   suite_add_tcase(s, tc_vcode);

   TCase *tc_vhdl = nvc_unit_test("vhdl");
   nvc_add_bytecode_fixture(tc_vhdl);
   tcase_add_test(tc_vhdl, test_compile_fact);
   tcase_add_test(tc_vhdl, test_compile_hello);
   suite_add_tcase(s, tc_vhdl);

   return s;
}
