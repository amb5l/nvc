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

#pragma once

#include "bytecode.hpp"
#include "util/bitmask.hpp"

#include <map>
#include <set>
#include <climits>

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
   void compile_uarray_left(int op);
   void compile_uarray_right(int op);
   void compile_uarray_dir(int op);
   void compile_unwrap(int op);
   void compile_cast(int op);
   void compile_range_null(int op);
   void compile_select(int op);

   int size_of(vcode_type_t vtype) const;
   Bytecode::Label &label_for_block(vcode_block_t block);
   void spill_live();
   void find_def_use();

   struct Location {
      static Location invalid() { return Location { VCODE_INVALID_BLOCK, -1 }; }
      static Location global() {
         return Location { VCODE_INVALID_BLOCK, INT_MAX };
      }

      bool operator==(const Location& l) const {
         return l.block == block && l.op == op;
      }
      bool operator!=(const Location& l) const { return !(*this == l); }

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
