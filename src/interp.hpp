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

#include "util.h"
#include "util/crashdump.hpp"
#include "bytecode.hpp"

class Interpreter : private CrashHandler {
public:
   Interpreter();

   typedef int32_t reg_t;

   reg_t run(const Bytecode *code);
   reg_t get_reg(unsigned num) const;
   void set_reg(unsigned num, reg_t value);
   void push(uint32_t word);
   uint32_t pop();
   void reset();
   void dump();

private:
   Interpreter(const Interpreter&) = delete;
   Interpreter(Interpreter&&) = delete;

   void on_crash() override;

   inline Bytecode::OpCode opcode();
   inline uint8_t reg();
   inline int8_t imm8();
   inline int16_t imm16();
   inline uint32_t& mem_access(int reg, int offset, int size);

   static const int STACK_SIZE = 256;
   static const int MEM_SIZE   = 1024;

   static_assert(STACK_SIZE < MEM_SIZE, "stack must be smaller than memory");

   /*class Frame {
      const Bytecode *bytecode;
      unsigned        bci;
      };*/

   const Bytecode *bytecode_ = nullptr;
   unsigned        bci_ = 0;
   unsigned        last_bci_ = 0;
   const uint8_t  *bytes_ = nullptr;
   reg_t           regs_[InterpMachine::NUM_REGS];
   uint8_t         flags_ = 0;
   uint32_t        mem_[MEM_SIZE / InterpMachine::WORD_SIZE];
};
