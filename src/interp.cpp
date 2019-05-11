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

#include "interp.hpp"

#include <cassert>

static inline uint8_t interp_cmp(Interpreter::reg_t lhs, Interpreter::reg_t rhs)
{
   uint8_t flags = 0;
   if (lhs == rhs)
      flags |= Bytecode::EQ;
   if (lhs != rhs)
      flags |= Bytecode::NE;
   if (lhs < rhs)
      flags |= Bytecode::LT;
   if (lhs <= rhs)
      flags |= Bytecode::LE;
   if (lhs > rhs)
      flags |= Bytecode::GT;
   if (lhs >= rhs)
      flags |= Bytecode::GE;
   return flags;
}

inline Bytecode::OpCode Interpreter::opcode()
{
   return (Bytecode::OpCode)bytes_[bci_++];
}

inline uint8_t Interpreter::reg()
{
   return bytes_[bci_++];
}

inline int8_t Interpreter::imm8()
{
   return bytes_[bci_++];
}

inline int16_t Interpreter::imm16()
{
    int16_t res = bytes_[bci_] | (bytes_[bci_ + 1] << 8);
    bci_ += 2;
    return res;
}

Interpreter::reg_t Interpreter::run(const Bytecode *code)
{
   bytecode_ = code;
   bytes_ = code->bytes();
   bci_ = 0;

   int32_t stack[1024];  // XXX

   int32_t a, b, c;
   for (;;) {
      switch (opcode()) {
      case Bytecode::ADDB:
         a = reg();
         b = imm8();
         regs_[a] += b;
         break;

      case Bytecode::RET:
         return regs_[0];

      case Bytecode::MOVB:
         a = reg();
         b = imm8();
         regs_[a] = b;
         break;

      case Bytecode::MOV:
         a = reg();
         b = reg();
         regs_[a] = regs_[b];
         break;

      case Bytecode::STR:
         (void)imm8(); // TODO
         b = imm16();
         c = reg();
         stack[b] = regs_[c];
         break;

      case Bytecode::LDR:
         a = reg();
         (void)imm8(); // TODO
         c = imm16();
         regs_[a] = stack[c];
         break;

      case Bytecode::CMP:
         a = reg();
         b = reg();
         flags_ = interp_cmp(regs_[a], regs_[b]);
         break;

      case Bytecode::CSET:
         a = reg();
         b = imm8();
         regs_[a] = !!(flags_ & b);
         break;

      case Bytecode::JMP:
         a = imm16();
         bci_ += a - 2;
         break;

      case Bytecode::JMPC:
         a = imm8();
         b = imm16();
         if (flags_ & a)
            bci_ += b - 2;
         break;

      case Bytecode::MUL:
         a = reg();
         b = reg();
         regs_[a] *= regs_[b];
         break;

      default:
         DEBUG_ONLY(code->dump();)
         should_not_reach_here("unhandled bytecode %02x at bci %d",
                               bytes_[bci_ - 1], bci_ - 1);
      }
   }
}

Interpreter::reg_t Interpreter::get_reg(unsigned num) const
{
   assert(num < InterpMachine::NUM_REGS);
   return regs_[num];
}

void Interpreter::set_reg(unsigned num, reg_t value)
{
   assert(num < InterpMachine::NUM_REGS);
   regs_[num] = value;
}
