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
#include "bytecode.hpp"

class Interpreter {
public:

   typedef int32_t reg_t;

   reg_t run(const Bytecode *code);
   reg_t get_reg(unsigned num) const;
   void set_reg(unsigned num, reg_t value);

private:
   inline Bytecode::OpCode opcode();
   inline uint8_t reg();
   inline int8_t imm8();
   inline int16_t imm16();

   /*class Frame {
      const Bytecode *bytecode;
      unsigned        bci;
      };*/

   const Bytecode *bytecode_ = nullptr;
   unsigned        bci_ = 0;
   const uint8_t  *bytes_ = nullptr;
   reg_t           regs_[InterpMachine::NUM_REGS];
   uint8_t         flags_ = 0;
};
