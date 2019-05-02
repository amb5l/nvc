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
     asm_(m),
     allocated_(m.num_regs())
{

}

Compiler::Mapping& Compiler::map_vcode_reg(vcode_reg_t reg)
{
   assert(reg < (int)reg_map_.size());
   return reg_map_[reg];
}

Compiler::Mapping& Compiler::map_vcode_var(vcode_var_t var)
{
   auto it = var_map_.find(var);
   assert(it != var_map_.end());
   return it->second;
}

Bytecode::Register Compiler::alloc_reg(Mapping& m, bool dirty)
{
   int free = allocated_.first_clear();
   if (free == -1)
      fatal_trace("out of registers");

   allocated_.set(free);
   live_.insert(&m);

   m.promote(Bytecode::R(free), dirty);

   return Bytecode::R(free);
}

Bytecode::Register Compiler::in_reg(Mapping& m)
{
   if (m.promoted())
      return m.reg();

   switch (m.storage()) {
   case Mapping::STACK:
      {
         if (current_location() != m.def()) {
            Bytecode::Register r = alloc_reg(m, false);
            __ comment("Unspill");
            __ ldr(r, Bytecode::R(machine_.sp_reg()), m.stack_slot());
            return r;
         }
         else
            return alloc_reg(m, true);
      }
   case Mapping::CONSTANT:
      {
         const int64_t value = m.constant();
         Bytecode::Register r = alloc_reg(m, false);
         __ comment("Materialise constant");
         __ mov(r, value);
         return r;
      }
   case Mapping::FLAGS:
      {
         Bytecode::Register r = alloc_reg(m, false);
         __ comment("Preserve flags");
         __ cset(r, m.cond());
         return r;
      }
   case Mapping::UNALLOCATED:
   default:
      should_not_reach_here("unallocated");
   }
}

bool Compiler::will_clobber_flags(int op)
{
   switch (vcode_get_op(op)) {
   default:
      return true;
   }
}

Bytecode::Condition Compiler::map_condition() const
{
   switch (vcode_get_cmp(op_)) {
   case VCODE_CMP_EQ:  return Bytecode::EQ;
   case VCODE_CMP_NEQ: return Bytecode::NE;
   case VCODE_CMP_LT : return Bytecode::LT;
   case VCODE_CMP_LEQ: return Bytecode::LE;
   case VCODE_CMP_GT : return Bytecode::GT;
   case VCODE_CMP_GEQ: return Bytecode::GE;
   default:
      should_not_reach_here("unhandled vcode comparison");
   }
}

Compiler::Location Compiler::current_location() const
{
   return Location { vcode_active_block(), op_ };
}

int Compiler::size_of(vcode_type_t vtype) const
{
   switch (vtype_kind(vtype)) {
   case VCODE_TYPE_INT:
   case VCODE_TYPE_OFFSET:
      return 4;
   case VCODE_TYPE_UARRAY:
      return 12; /* XXX ? */
   case VCODE_TYPE_POINTER:
      return 4; /* XXX ? */
   default:
      should_not_reach_here("unhandled type");
   }
}

void Compiler::spill_live()
{
   Location loc = current_location();

   for (Mapping* m : live_) {
      assert(m->promoted());

      if (m->storage() == Mapping::STACK && !m->dead(loc) && m->dirty()) {
         assert(m->size() == 4);
         __ comment("Spill");
         __ str(Bytecode::R(machine_.sp_reg()), m->stack_slot(), m->reg());
      }

      m->demote();
   }

   allocated_.zero();
   live_.clear();
}

void Compiler::find_def_use()
{
   const int nblocks = vcode_count_blocks();
   for (int i = 0; i < nblocks; i++) {
      vcode_select_block(i);

      const int nops = vcode_count_ops();
      for (int j = 0; j < nops; j++) {

         const int nargs = vcode_count_args(j);
         for (int k = 0; k < nargs; k++)
            reg_map_[vcode_get_arg(j, k)].use(Location { i, j });

         switch (vcode_get_op(j)) {
         case VCODE_OP_CONST:
         case VCODE_OP_ADDI:
         case VCODE_OP_CMP:
         case VCODE_OP_SELECT:
         case VCODE_OP_CAST:
         case VCODE_OP_LOAD:
         case VCODE_OP_MUL:
         case VCODE_OP_UARRAY_LEFT:
         case VCODE_OP_UARRAY_RIGHT:
         case VCODE_OP_UARRAY_DIR:
         case VCODE_OP_RANGE_NULL:
            reg_map_[vcode_get_result(j)].def(Location { i, j });
            break;

         case VCODE_OP_JUMP:
         case VCODE_OP_COND:
         case VCODE_OP_STORE:
         case VCODE_OP_RETURN:
         case VCODE_OP_BOUNDS:
         case VCODE_OP_COMMENT:
         case VCODE_OP_DEBUG_INFO:
         case VCODE_OP_DYNAMIC_BOUNDS:
            break;

         default:
            DEBUG_ONLY(vcode_dump_with_mark(j));
            should_not_reach_here("cannot analyse vcode op %s",
                                  vcode_op_string(vcode_get_op(j)));
         }
      }
   }
}

bool Compiler::can_use_flags(const Mapping& m)
{
   assert(vcode_get_op(op_) == VCODE_OP_CMP);

   Location scan = current_location();
   assert(scan == m.def());

   const Location last_use = m.last_use();
   if (last_use == Location::global() || last_use == Location::invalid())
      return false;

   assert(last_use.block == scan.block && last_use.op > scan.op);

   scan.op++;
   while (scan != last_use) {
      if (will_clobber_flags(scan.op))
         return false;

      scan.op++;
   }

   return true;
}

Bytecode *Compiler::compile(vcode_unit_t unit)
{
   vcode_select_unit(unit);

   int stack_offset = 0;
   const int nvars = vcode_count_vars();
   for (int i = 0; i < nvars; i++) {
      vcode_var_t var = vcode_var_handle(i);
      Mapping m(Mapping::VAR, 4 /* XXX */);
      m.make_stack(stack_offset);

      var_map_.emplace(var, m);
      stack_offset += 4;
   }

   const int nregs = vcode_count_regs();
   for (int i = 0; i < nregs; i++)
      reg_map_.emplace_back(Mapping(Mapping::TEMP, size_of(vcode_reg_type(i))));

   const int nparams = vcode_count_params();
   for (int i = 0; i < nparams; i++) {
      Mapping& m = reg_map_[i];
      m.make_stack(stack_offset);
      m.promote(Bytecode::R(i), true);
      m.def(Location { 0, 0 });
      stack_offset += m.size();
   }

   find_def_use();

   const int nblocks = vcode_count_blocks();
   for (int i = 0; i < nblocks; i++) {
      vcode_select_block(i);

      const int nops = vcode_count_ops();
      for (int j = 0; j < nops; j++) {
         op_ = j;

         switch (vcode_get_op(j)) {
         case VCODE_OP_CONST:
            {
               Mapping& m = reg_map_[vcode_get_result(j)];
               const int64_t value = vcode_get_value(j);
               if (is_int8(value))
                  m.make_constant(value);
            }
            break;

         case VCODE_OP_CMP:
            {
               Mapping& m = reg_map_[vcode_get_result(j)];
               if (can_use_flags(m))
                  m.make_flags(map_condition());
               else {
                  m.make_stack(stack_offset);
                  stack_offset += m.size();
               }
            }
            break;

         case VCODE_OP_ADDI:
         case VCODE_OP_SELECT:
         case VCODE_OP_CAST:
         case VCODE_OP_LOAD:
         case VCODE_OP_MUL:
         case VCODE_OP_UARRAY_LEFT:
         case VCODE_OP_UARRAY_RIGHT:
         case VCODE_OP_UARRAY_DIR:
         case VCODE_OP_RANGE_NULL:
            {
               Mapping& m = reg_map_[vcode_get_result(j)];
               m.make_stack(stack_offset);
               stack_offset += m.size();
            }
            break;

         case VCODE_OP_JUMP:
         case VCODE_OP_COND:
         case VCODE_OP_STORE:
         case VCODE_OP_RETURN:
         case VCODE_OP_BOUNDS:
         case VCODE_OP_COMMENT:
         case VCODE_OP_DEBUG_INFO:
         case VCODE_OP_DYNAMIC_BOUNDS:
            break;

         default:
            DEBUG_ONLY(vcode_dump_with_mark(j));
            should_not_reach_here("cannot analyse vcode op %s",
                                  vcode_op_string(vcode_get_op(j)));
         }
      }
   }

   __ set_frame_size(stack_offset);

   for (int i = 0; i < nblocks; i++)
      block_map_.push_back(Bytecode::Label());

   // Parameters are all live on entry
   for (int i = 0; i < nparams; i++) {
      live_.insert(&(reg_map_[i]));
      allocated_.set(i);
   }

   for (int i = 0; i < nblocks; i++) {
      vcode_select_block(i);

      __ bind(block_map_[i]);
      __ comment("Block entry %d", i);

      const int nops = vcode_count_ops();
      for (int j = 0; j < nops; j++) {
         op_ = j;

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

      assert(allocated_.all_clear());
      assert(live_.empty());
   }

   block_map_.clear();  // Check all labels are bound

   return __ finish();
}

void Compiler::compile_const(int op)
{
   Mapping& result = map_vcode_reg(vcode_get_result(op));
   if (result.storage() != Mapping::CONSTANT) {
      __ mov(in_reg(result), vcode_get_value(op));
   }
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
   Bytecode::Register dst = in_reg(map_vcode_reg(vcode_get_result(op)));
   Bytecode::Register src = in_reg(map_vcode_reg(vcode_get_arg(op, 0)));

   __ mov(dst, src);
   __ add(dst, vcode_get_value(op));
}

void Compiler::compile_return(int op)
{
   Bytecode::Register value = in_reg(map_vcode_reg(vcode_get_arg(op, 0)));

   if (value != Bytecode::R(machine_.result_reg())) {
      __ mov(Bytecode::R(machine_.result_reg()), value);
   }

   __ ret();

   live_.clear();
   allocated_.zero();
}

void Compiler::compile_store(int op)
{
   Mapping& dst = map_vcode_var(vcode_get_address(op));
   Mapping& src = map_vcode_reg(vcode_get_arg(op, 0));

   __ str(Bytecode::R(machine_.sp_reg()), dst.stack_slot(), in_reg(src));
}

void Compiler::compile_load(int op)
{
   Mapping& src = map_vcode_var(vcode_get_address(op));
   Mapping& dst = map_vcode_reg(vcode_get_result(op));

   __ ldr(in_reg(dst), Bytecode::R(machine_.sp_reg()), src.stack_slot());
}

void Compiler::compile_cmp(int op)
{
   Mapping& dst = map_vcode_reg(vcode_get_result(op));
   Bytecode::Register lhs = in_reg(map_vcode_reg(vcode_get_arg(op, 0)));
   Bytecode::Register rhs = in_reg(map_vcode_reg(vcode_get_arg(op, 1)));

   __ cmp(lhs, rhs);
   if (dst.storage() != Mapping::FLAGS) {
      __ cset(in_reg(dst), map_condition());
   }
}

void Compiler::compile_cond(int op)
{
   Mapping& test = map_vcode_reg(vcode_get_arg(op, 0));

   if (test.storage() == Mapping::FLAGS) {
      spill_live();
      __ jmp(label_for_block(vcode_get_target(op, 0)), test.cond());
   }
   else {
      Bytecode::Register src = in_reg(test);
      spill_live();
      __ cbnz(src, label_for_block(vcode_get_target(op, 0)));
   }

   __ jmp(label_for_block(vcode_get_target(op, 1)));
}

void Compiler::compile_jump(int op)
{
   spill_live();

   __ jmp(label_for_block(vcode_get_target(op, 0)));
}

void Compiler::compile_mul(int op)
{
   Bytecode::Register dst = in_reg(map_vcode_reg(vcode_get_result(op)));
   Bytecode::Register lhs = in_reg(map_vcode_reg(vcode_get_arg(op, 0)));
   Bytecode::Register rhs = in_reg(map_vcode_reg(vcode_get_arg(op, 1)));

   __ mov(dst, lhs);
   __ mul(dst, rhs);
}

void Compiler::compile_select(int op)
{
   Mapping& sel = map_vcode_reg(vcode_get_arg(op, 0));
   Bytecode::Register dst = in_reg(map_vcode_reg(vcode_get_result(op)));
   Bytecode::Register lhs = in_reg(map_vcode_reg(vcode_get_arg(op, 1)));
   Bytecode::Register rhs = in_reg(map_vcode_reg(vcode_get_arg(op, 2)));

   __ mov(dst, lhs);
   Bytecode::Label skip;
   if (sel.storage() == Mapping::FLAGS) {
      __ jmp(skip, sel.cond());
   }
   else {
      __ cbz(in_reg(sel), skip);
   }
   __ mov(dst, rhs);
   __ bind(skip);
}

Bytecode::Label& Compiler::label_for_block(vcode_block_t block)
{
   assert(block < (int)block_map_.size());
   return block_map_[block];
}

Compiler::Mapping::Mapping(Kind kind, int size)
   : size_(size),
     kind_(kind),
     storage_(UNALLOCATED)
{

}

Bytecode::Register Compiler::Mapping::reg() const
{
   assert(promoted_);
   return reg_;
}

int Compiler::Mapping::stack_slot() const
{
   assert(storage_ == STACK);
   return stack_slot_;
}

int64_t Compiler::Mapping::constant() const
{
   assert(storage_ == CONSTANT);
   return constant_;
}

Bytecode::Condition Compiler::Mapping::cond() const
{
   assert(storage_ == FLAGS);
   return cond_;
}

void Compiler::Mapping::promote(Bytecode::Register reg, bool dirty)
{
   assert(!promoted_);
   promoted_ = true;
   reg_ = reg;
   dirty_ = dirty;
}

void Compiler::Mapping::make_stack(int offset)
{
   assert(storage_ == UNALLOCATED);
   storage_ = STACK;
   stack_slot_ = offset;
}

void Compiler::Mapping::make_constant(int64_t value)
{
   assert(storage_ == UNALLOCATED);
   storage_ = CONSTANT;
   constant_ = value;
}

void Compiler::Mapping::make_flags(Bytecode::Condition cond)
{
   assert(storage_ == UNALLOCATED);
   storage_ = FLAGS;
   cond_ = cond;
}

void Compiler::Mapping::demote()
{
   assert(promoted_);
   promoted_ = false;
}

void Compiler::Mapping::def(Location loc)
{
   assert(def_ == Location::invalid());
   def_ = loc;
}

void Compiler::Mapping::use(Location loc)
{
   assert(def_ != Location::invalid());

   if (def_.block != loc.block)
      last_use_ = Location::global();
   else if (loc.op > last_use_.op)
      last_use_ = loc;
}

bool Compiler::Mapping::dead(Location loc) const
{
   if (def_.block == last_use_.block)
      return loc.op <= def_.op || loc.op >= last_use_.op;
   else
      return false;
}

bool Compiler::Mapping::dirty() const
{
   assert(promoted_);
   return dirty_;
}
