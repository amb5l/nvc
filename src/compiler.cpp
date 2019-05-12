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

#include "bytecode.hpp"
#include "util/bitmask.hpp"
#include "phase.h"
#include "common.h"

#include <vector>
#include <map>
#include <set>
#include <cassert>
#include <cstring>
#include <ostream>
#include <climits>

#define __ asm_.

namespace {

   class Compiler {
   public:
      explicit Compiler(const Machine &m);
      Compiler(const Compiler &) = delete;

      Bytecode *compile(vcode_unit_t unit);

   private:
      void compile_const(int op);
      void compile_addi(int op);
      void compile_return(int op);
      void compile_store(int op);
      void compile_cmp(int op);
      void compile_load(int op);
      void compile_jump(int op);
      void compile_cond(int op);
      void compile_mul(int op);
      void compile_sub();
      void compile_add();
      void compile_uarray_left();
      void compile_uarray_right();
      void compile_uarray_dir();
      void compile_unwrap();
      void compile_cast();
      void compile_range_null();
      void compile_select(int op);
      void compile_load_indirect();

      int size_of(vcode_type_t vtype) const;
      Bytecode::Label &label_for_block(vcode_block_t block);
      void spill_live();
      void find_def_use();

      struct Location {
         static Location invalid() { return Location{VCODE_INVALID_BLOCK, -1}; }
         static Location global() {
            return Location{VCODE_INVALID_BLOCK, INT_MAX};
         }

         bool operator==(const Location &l) const {
            return l.block == block && l.op == op;
         }
         bool operator!=(const Location &l) const { return !(*this == l); }

         vcode_block_t block;
         int op;
      };

      class Mapping {
      public:
         enum Kind { VAR, PARAM, TEMP };

         explicit Mapping(Kind kind, int size);
         Mapping(const Mapping&) = default;

         void make_stack(int slot);
         void make_constant(int64_t value);
         void make_flags(Bytecode::Condition cond);

         enum Storage { UNALLOCATED, STACK, CONSTANT, FLAGS };

         Kind kind() const { return kind_; }
         Storage storage() const { return storage_; }
         int size() const { return size_; }
         int stack_slot() const;
         Bytecode::Condition cond() const;
         Bytecode::Register reg() const;
         int64_t constant() const;
         void promote(Bytecode::Register reg, bool dirty);
         bool promoted() const { return promoted_; }
         void demote();
         void def(Location loc);
         void use(Location loc);
         bool dead(Location loc) const;
         Location def() const { return def_; }
         Location last_use() const { return last_use_; }
         bool dirty() const;

      private:
         int                size_;
         bool               promoted_ = false;
         Bytecode::Register reg_;
         Kind               kind_;
         Location           def_ = Location::invalid();
         Location           last_use_ = Location::invalid();
         bool               dirty_ = false;

         Storage storage_;
         union {
            int                 stack_slot_;
            int64_t             constant_;
            Bytecode::Condition cond_;
         };
      };

      struct Patch {
         vcode_block_t block;
         Bytecode::Label label;
      };

      Bytecode::Register in_reg(Mapping& m);
      Bytecode::Register in_reg(Mapping& m, Mapping& reuse);
      Bytecode::Register in_reg(Mapping& m, Bytecode::Register reuse);
      Bytecode::Register alloc_reg(Mapping& m, bool dirty);
      bool can_use_flags(const Mapping& m);
      Bytecode::Condition map_condition() const;

      static bool will_clobber_flags(int op);

      Location current_location() const;

      Mapping &map_vcode_reg(vcode_reg_t reg);
      Mapping &map_vcode_var(vcode_reg_t reg);

      const Machine                  machine_;
      Bytecode::Assembler            asm_;
      std::map<vcode_var_t, Mapping> var_map_;
      std::vector<Mapping>           reg_map_;
      std::vector<Bytecode::Label>   block_map_;
      std::set<Mapping*>             live_;
      Bitmask                        allocated_;
      int                            op_ = -1;
   };

}

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

Bytecode::Register Compiler::in_reg(Mapping& m, Mapping& reuse)
{
   if (m.promoted())
      return m.reg();

   if (reuse.promoted() && reuse.last_use() == current_location()) {
      Bytecode::Register r = reuse.reg();
      reuse.demote();
      live_.erase(&reuse);
      live_.insert(&m);
      m.promote(r, true);
      return r;
   }

   return in_reg(m);
}

Bytecode::Register Compiler::in_reg(Mapping& m, Bytecode::Register reuse)
{
   if (allocated_.is_set(reuse.num)) {
      for (Mapping* it : live_) {
         if (it->reg() == reuse)
            return in_reg(m, *it);
      }

      should_not_reach_here("allocated reg not in live set");
   }
   else {
      m.promote(reuse, current_location() == m.def());
      live_.insert(&m);
      allocated_.set(reuse.num);
      return reuse;
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
      return machine_.word_size() + 4 + 8 * vtype_dims(vtype);
   case VCODE_TYPE_POINTER:
      return machine_.word_size();
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
         assert(m->size() == machine_.word_size());
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

         vcode_reg_t result = vcode_get_result(j);
         if (result != VCODE_INVALID_REG)
            reg_map_[result].def(Location { i, j });
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
      if (m.size() <= machine_.word_size()) {
         m.promote(Bytecode::R(i), true);
         live_.insert(&(reg_map_[i]));
         allocated_.set(i);
      }
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

         vcode_reg_t result = vcode_get_result(j);
         if (result == VCODE_INVALID_REG)
            continue;

         Mapping& m = reg_map_[result];

         switch (vcode_get_op(j)) {
         case VCODE_OP_CONST:
            {
               const int64_t value = vcode_get_value(j);
               if (is_int8(value))
                  m.make_constant(value);
            }
            break;

         case VCODE_OP_CMP:
            if (can_use_flags(m))
               m.make_flags(map_condition());
            else {
               m.make_stack(stack_offset);
               stack_offset += m.size();
            }
            break;

         default:
            m.make_stack(stack_offset);
            stack_offset += m.size();
         }
      }
   }

   __ set_frame_size(stack_offset);

   for (int i = 0; i < nblocks; i++)
      block_map_.push_back(Bytecode::Label());

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
         case VCODE_OP_SUB:
            compile_sub();
            break;
         case VCODE_OP_ADD:
            compile_add();
            break;
         case VCODE_OP_COND:
            compile_cond(j);
            break;
         case VCODE_OP_UARRAY_LEFT:
            compile_uarray_left();
            break;
         case VCODE_OP_UARRAY_RIGHT:
            compile_uarray_right();
            break;
         case VCODE_OP_UARRAY_DIR:
            compile_uarray_dir();
            break;
         case VCODE_OP_CAST:
            compile_cast();
            break;
         case VCODE_OP_RANGE_NULL:
            compile_range_null();
            break;
         case VCODE_OP_SELECT:
            compile_select(j);
            break;
         case VCODE_OP_UNWRAP:
            compile_unwrap();
            break;
         case VCODE_OP_LOAD_INDIRECT:
            compile_load_indirect();
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

void Compiler::compile_unwrap()
{
   Mapping& uarray = map_vcode_reg(vcode_get_arg(op_, 0));
   assert(uarray.storage() == Mapping::STACK);

   Bytecode::Register dst = in_reg(map_vcode_reg(vcode_get_result(op_)));

   __ ldr(dst, Bytecode::R(machine_.sp_reg()), uarray.stack_slot());
}

void Compiler::compile_cast()
{
   vcode_reg_t arg_reg    = vcode_get_arg(op_, 0);
   vcode_reg_t result_reg = vcode_get_result(op_);

   vcode_type_t arg_type    = vcode_reg_type(arg_reg);
   vcode_type_t result_type = vcode_reg_type(result_reg);

   vtype_kind_t arg_kind    = vtype_kind(arg_type);
   vtype_kind_t result_kind = vtype_kind(result_type);

   if (arg_kind == VCODE_TYPE_CARRAY) {
      // This is a no-op as constrained arrays are implemented as pointers
      should_not_reach_here("todo");
   }
   else if (result_kind == VCODE_TYPE_REAL && arg_kind == VCODE_TYPE_INT) {
      should_not_reach_here("todo");
   }
   else if (result_kind == VCODE_TYPE_INT && arg_kind == VCODE_TYPE_REAL) {
      should_not_reach_here("todo");
   }
   else if (result_kind == VCODE_TYPE_INT || result_kind == VCODE_TYPE_OFFSET) {
      const int abits = bits_for_range(vtype_low(arg_type),
                                       vtype_high(arg_type));
      const int rbits = bits_for_range(vtype_low(result_type),
                                       vtype_high(result_type));

      Bytecode::Register arg = in_reg(map_vcode_reg(arg_reg));
      Bytecode::Register result = in_reg(map_vcode_reg(result_reg), arg);

      if (rbits < abits) {
         __ comment("TODO: truncate %d -> %d", abits, rbits);
         __ nop();
      }
      else if (vtype_low(arg_type) < 0 && abits != rbits) {
         __ comment("TODO: sign extend %d -> %d", abits, rbits);
         __ nop();
      }
      else {
         __ mov(result, arg);
      }
   }
   else
      should_not_reach_here("unexpected");
}

void Compiler::compile_range_null()
{
   Bytecode::Label Ldone, Ldownto;

   Bytecode::Register left  = in_reg(map_vcode_reg(vcode_get_arg(op_, 0)));
   Bytecode::Register right = in_reg(map_vcode_reg(vcode_get_arg(op_, 1)));
   Bytecode::Register dir   = in_reg(map_vcode_reg(vcode_get_arg(op_, 2)));
   Bytecode::Register dst   = in_reg(map_vcode_reg(vcode_get_result(op_)));

   __ comment("Null range check");
   __ test(dir, 1);
   __ jmp(Ldownto, Bytecode::EQ);
   __ cmp(left, right);
   __ cset(dst, Bytecode::GT);
   __ jmp(Ldone);
   __ bind(Ldownto);
   __ cmp(left, right);
   __ cset(dst, Bytecode::LT);
   __ bind(Ldone);
}

void Compiler::compile_uarray_left()
{
   vcode_reg_t arg_reg = vcode_get_arg(op_, 0);
   Mapping& uarray = map_vcode_reg(arg_reg);
   assert(uarray.storage() == Mapping::STACK);

   Bytecode::Register dst = in_reg(map_vcode_reg(vcode_get_result(op_)));

   const size_t offs = uarray.stack_slot() + machine_.word_size() + 4;
   __ ldr(dst, Bytecode::R(machine_.sp_reg()), offs + 4 * vcode_get_dim(op_));
}

void Compiler::compile_uarray_right()
{
   vcode_reg_t arg_reg = vcode_get_arg(op_, 0);
   Mapping& uarray = map_vcode_reg(arg_reg);
   assert(uarray.storage() == Mapping::STACK);

   Bytecode::Register dst = in_reg(map_vcode_reg(vcode_get_result(op_)));

   const size_t offs = uarray.stack_slot() + machine_.word_size() + 8;
   __ ldr(dst, Bytecode::R(machine_.sp_reg()), offs + 4 * vcode_get_dim(op_));
}

void Compiler::compile_uarray_dir()
{
   vcode_reg_t arg_reg = vcode_get_arg(op_, 0);
   Mapping& uarray = map_vcode_reg(arg_reg);
   assert(uarray.storage() == Mapping::STACK);

   Bytecode::Register dst = in_reg(map_vcode_reg(vcode_get_result(op_)));

   const size_t offs = uarray.stack_slot() + machine_.word_size();
   __ ldr(dst, Bytecode::R(machine_.sp_reg()), offs);

   const unsigned dim = vcode_get_dim(op_);
   if (vtype_dims(vcode_reg_type(arg_reg)) > 1) {
      assert(dim < 32);
      __ test(dst, 1 << dim);
      __ cset(dst, Bytecode::NE);
   }
   else
      assert(dim == 0);
}

void Compiler::compile_load_indirect()
{
   Bytecode::Register src = in_reg(map_vcode_reg(vcode_get_arg(op_, 0)));
   Bytecode::Register dst = in_reg(map_vcode_reg(vcode_get_result(op_)), src);

   __ ldr(dst, src, 0);
}

void Compiler::compile_addi(int op)
{
   vcode_type_t result_type = vcode_reg_type(vcode_get_result(op_));

   Bytecode::Register src = in_reg(map_vcode_reg(vcode_get_arg(op_, 0)));
   Bytecode::Register dst = in_reg(map_vcode_reg(vcode_get_result(op_)), src);

   __ mov(dst, src);

   int64_t value = vcode_get_value(op);
   if (vtype_kind(result_type) == VCODE_TYPE_POINTER)
      value *= size_of(result_type);

   __ add(dst, value);
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
      __ test(src, 1);
      __ jmp(label_for_block(vcode_get_target(op, 0)), Bytecode::NZ);
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
   Mapping& tmp = map_vcode_reg(vcode_get_arg(op_, 0));

   Bytecode::Register lhs = in_reg(tmp);
   Bytecode::Register rhs = in_reg(map_vcode_reg(vcode_get_arg(op_, 1)));
   Bytecode::Register dst = in_reg(map_vcode_reg(vcode_get_result(op_)), tmp);

   __ mov(dst, lhs);
   __ mul(dst, rhs);
}

void Compiler::compile_sub()
{
   Mapping& tmp = map_vcode_reg(vcode_get_arg(op_, 0));

   Bytecode::Register lhs = in_reg(tmp);
   Bytecode::Register rhs = in_reg(map_vcode_reg(vcode_get_arg(op_, 1)));
   Bytecode::Register dst = in_reg(map_vcode_reg(vcode_get_result(op_)), tmp);

   __ mov(dst, lhs);
   __ sub(dst, rhs);
}

void Compiler::compile_add()
{
   vcode_reg_t result_reg = vcode_get_result(op_);
   vcode_type_t result_type = vcode_reg_type(result_reg);

   Bytecode::Register lhs = in_reg(map_vcode_reg(vcode_get_arg(op_, 0)));
   Bytecode::Register rhs = in_reg(map_vcode_reg(vcode_get_arg(op_, 1)));

   if (vtype_kind(result_type) == VCODE_TYPE_POINTER) {
      Bytecode::Register dst = in_reg(map_vcode_reg(result_reg), rhs);
      __ mov(dst, rhs);
      __ mul(dst, size_of(result_type));
      __ add(dst, lhs);
   }
   else {
      Bytecode::Register dst = in_reg(map_vcode_reg(result_reg), lhs);
      __ mov(dst, lhs);
      __ add(dst, rhs);
   }
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
      __ test(in_reg(sel), 1);
      __ jmp(skip, Bytecode::NZ);
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

Bytecode *compile(const Machine& m, vcode_unit_t unit)
{
   return Compiler(m).compile(unit);
}
