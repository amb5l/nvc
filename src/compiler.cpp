//
//  Copyright (C) 2019  Nick Gasson
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include "compiler.hpp"

#include <vector>
#include <map>
#include <assert.h>
#include <string.h>
#include <ostream>

#define __ asm_.

Compiler::Compiler(const Machine& m)
   : machine_(m),
     asm_(m)
{

}

const Compiler::Mapping& Compiler::map_vcode_reg(vcode_reg_t reg) const
{
   assert(reg < (int)reg_map_.size());
   return reg_map_[reg];
}

const Compiler::Mapping& Compiler::map_vcode_var(vcode_var_t var) const
{
   auto it = var_map_.find(var);
   assert(it != var_map_.end());
   return it->second;
}

Bytecode *Compiler::compile(vcode_unit_t unit)
{
   vcode_select_unit(unit);

   int stack_offset = 0;
   const int nvars = vcode_count_vars();
   for (int i = 0; i < nvars; i++) {
      vcode_var_t var = vcode_var_handle(i);
      Mapping m = { Mapping::STACK, stack_offset };
      var_map_[var] = m;
      stack_offset += 4;
   }

   const int nregs = vcode_count_regs();
   for (int i = 0; i < nregs; i++) {
      switch (vcode_reg_kind(i)) {
      case VCODE_TYPE_INT:
      case VCODE_TYPE_OFFSET:
      case VCODE_TYPE_POINTER:
         reg_map_.push_back(Mapping {Mapping::REGISTER, {i}});
         break;

      case VCODE_TYPE_UARRAY:
         reg_map_.push_back(Mapping { Mapping::STACK, { stack_offset }});
         stack_offset += 16;  /* XXX */
         break;

      default:
         should_not_reach_here("cannot handle vcode type %d",
                               vcode_reg_kind(i));
      }
   }

   __ set_frame_size(stack_offset);

   const int nblocks = vcode_count_blocks();

   for (int i = 0; i < nblocks; i++)
      block_map_.push_back(Bytecode::Label());

   for (int i = 0; i < nblocks; i++) {
      vcode_select_block(i);

      __ bind(block_map_[i]);

      const int nops = vcode_count_ops();
      for (int j = 0; j < nops; j++) {
         switch (vcode_get_op(j)) {
         case VCODE_OP_CONST:
            compile_const(j);
            break;
         case VCODE_OP_ADDI:
            compile_addi(j);
            break;
         case VCODE_OP_RETURN:
            compile_return(j);
            break;
         case VCODE_OP_STORE:
            compile_store(j);
            break;
         case VCODE_OP_CMP:
            compile_cmp(j);
            break;
         case VCODE_OP_JUMP:
            compile_jump(j);
            break;
         case VCODE_OP_LOAD:
            compile_load(j);
            break;
         case VCODE_OP_MUL:
            compile_mul(j);
            break;
         case VCODE_OP_COND:
            compile_cond(j);
            break;
         case VCODE_OP_UARRAY_LEFT:
            compile_uarray_left(j);
            break;
         case VCODE_OP_UARRAY_RIGHT:
            compile_uarray_right(j);
            break;
         case VCODE_OP_UARRAY_DIR:
            compile_uarray_dir(j);
            break;
         case VCODE_OP_CAST:
            compile_cast(j);
            break;
         case VCODE_OP_RANGE_NULL:
            compile_range_null(j);
            break;
         case VCODE_OP_SELECT:
            compile_select(j);
            break;
         case VCODE_OP_BOUNDS:
         case VCODE_OP_COMMENT:
         case VCODE_OP_DEBUG_INFO:
         case VCODE_OP_DYNAMIC_BOUNDS:
            break;
         default:
            vcode_dump_with_mark(j);
            fatal("cannot compile vcode op %s to bytecode",
                  vcode_op_string(vcode_get_op(j)));
         }
      }
   }

   block_map_.clear();  // Check all labels are bound

   return __ finish();
}

void Compiler::compile_const(int op)
{
   const Mapping& result = map_vcode_reg(vcode_get_result(op));
   assert(result.kind == Mapping::REGISTER);

   __ mov(result.reg, vcode_get_value(op));
}

void Compiler::compile_cast(int op)
{
   __ nop();  // TODO
}

void Compiler::compile_range_null(int op)
{
   __ nop();  // TODO
}

void Compiler::compile_uarray_left(int op)
{
   __ nop();  // TODO
}

void Compiler::compile_uarray_right(int op)
{
   __ nop();  // TODO
}

void Compiler::compile_uarray_dir(int op)
{
   __ nop();  // TODO
}

void Compiler::compile_addi(int op)
{
   const Mapping& dst = map_vcode_reg(vcode_get_result(op));
   const Mapping& src = map_vcode_reg(vcode_get_arg(op, 0));

   assert(dst.kind == Mapping::REGISTER);
   assert(src.kind == Mapping::REGISTER);

   __ mov(dst.reg, src.reg);
   __ add(dst.reg, vcode_get_value(op));
}

void Compiler::compile_return(int op)
{
   const Mapping& value = map_vcode_reg(vcode_get_arg(op, 0));
   assert(value.kind == Mapping::REGISTER);

   if (value.slot != machine_.result_reg()) {
      __ mov(Bytecode::R(machine_.result_reg()), value.reg);
   }

   __ ret();
}

void Compiler::compile_store(int op)
{
   const Mapping& dst = map_vcode_var(vcode_get_address(op));
   assert(dst.kind == Mapping::STACK);

   const Mapping& src = map_vcode_reg(vcode_get_arg(op, 0));
   assert(src.kind == Mapping::REGISTER);

   __ str(Bytecode::R(machine_.sp_reg()), dst.slot, src.reg);
}

void Compiler::compile_load(int op)
{
   const Mapping& src = map_vcode_var(vcode_get_address(op));
   assert(src.kind == Mapping::STACK);

   const Mapping& dst = map_vcode_reg(vcode_get_result(op));
   assert(dst.kind == Mapping::REGISTER);

   __ ldr(dst.reg, Bytecode::R(machine_.sp_reg()), src.slot);
}

void Compiler::compile_cmp(int op)
{
   const Mapping& dst = map_vcode_reg(vcode_get_result(op));
   const Mapping& lhs = map_vcode_reg(vcode_get_arg(op, 0));
   const Mapping& rhs = map_vcode_reg(vcode_get_arg(op, 1));

   assert(dst.kind == Mapping::REGISTER);
   assert(lhs.kind == Mapping::REGISTER);
   assert(rhs.kind == Mapping::REGISTER);

   Bytecode::Condition cond = Bytecode::EQ;
   switch (vcode_get_cmp(op)) {
   case VCODE_CMP_EQ:  cond = Bytecode::EQ; break;
   case VCODE_CMP_NEQ: cond = Bytecode::NE; break;
   case VCODE_CMP_LT : cond = Bytecode::LT; break;
   case VCODE_CMP_LEQ: cond = Bytecode::LE; break;
   case VCODE_CMP_GT : cond = Bytecode::GT; break;
   case VCODE_CMP_GEQ: cond = Bytecode::GE; break;
   default:
      should_not_reach_here("unhandled vcode comparison");
   }

   __ cmp(lhs.reg, rhs.reg);
   __ cset(dst.reg, cond);
}

void Compiler::compile_cond(int op)
{
   const Mapping& src = map_vcode_reg(vcode_get_arg(op, 0));
   assert(src.kind == Mapping::REGISTER);

   __ cbnz(src.reg, label_for_block(vcode_get_target(op, 0)));

   __ jmp(label_for_block(vcode_get_target(op, 1)));
}

void Compiler::compile_jump(int op)
{
   __ jmp(label_for_block(vcode_get_target(op, 0)));
}

void Compiler::compile_mul(int op)
{
   const Mapping& dst = map_vcode_reg(vcode_get_result(op));
   const Mapping& lhs = map_vcode_reg(vcode_get_arg(op, 0));
   const Mapping& rhs = map_vcode_reg(vcode_get_arg(op, 1));

   assert(dst.kind == Mapping::REGISTER);
   assert(lhs.kind == Mapping::REGISTER);
   assert(rhs.kind == Mapping::REGISTER);

   __ mov(dst.reg, lhs.reg);
   __ mul(dst.reg, rhs.reg);
}

void Compiler::compile_select(int op)
{
   const Mapping& dst = map_vcode_reg(vcode_get_result(op));
   const Mapping& sel = map_vcode_reg(vcode_get_arg(op, 0));
   const Mapping& lhs = map_vcode_reg(vcode_get_arg(op, 1));
   const Mapping& rhs = map_vcode_reg(vcode_get_arg(op, 2));

   assert(dst.kind == Mapping::REGISTER);
   assert(sel.kind == Mapping::REGISTER);
   assert(lhs.kind == Mapping::REGISTER);
   assert(rhs.kind == Mapping::REGISTER);

   __ mov(dst.reg, lhs.reg);
   Bytecode::Label skip;
   __ cbz(sel.reg, skip);
   __ mov(dst.reg, rhs.reg);
   __ bind(skip);
}

Bytecode::Label& Compiler::label_for_block(vcode_block_t block)
{
   assert(block < (int)block_map_.size());
   return block_map_[block];
}
