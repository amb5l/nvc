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
#include <cstring>

Interpreter::Interpreter(RtCallHandler *handler)
   : handler_(handler ?: this)
{
   reset();
}

void Interpreter::reset()
{
   DEBUG_ONLY(memset(mem_, 0xde, sizeof(mem_));)
   DEBUG_ONLY(memset(regs_, 0xad, sizeof(regs_));)

   // Stack is at bottom of memory and grows downwards
   regs_[InterpMachine::SP_REG] = STACK_SIZE;
}

void Interpreter::push(uint32_t word)
{
   regs_[InterpMachine::SP_REG] -= InterpMachine::WORD_SIZE;
   assert(regs_[InterpMachine::SP_REG] >= 0);
   mem_wr(InterpMachine::SP_REG, 0) = word;
}

uint32_t Interpreter::pop()
{
   const uint32_t result = mem_rd(InterpMachine::SP_REG, 0);
   regs_[InterpMachine::SP_REG] += InterpMachine::WORD_SIZE;
   assert(regs_[InterpMachine::SP_REG] <= STACK_SIZE);
   return result;
}

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

static inline uint8_t interp_test(Interpreter::reg_t lhs,
                                  Interpreter::reg_t rhs)
{
   uint8_t flags = 0;
   if (lhs & rhs)
      flags |= Bytecode::NZ;
   else
      flags |= Bytecode::Z;
   return flags;
}

inline Bytecode::OpCode Interpreter::opcode()
{
   return (Bytecode::OpCode)bytes_[bci_++];
}

inline uint8_t Interpreter::reg()
{
   reg_t r = bytes_[bci_++];
   assert(r < InterpMachine::NUM_REGS);
   return r;
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

uint32_t& Interpreter::mem_wr(int reg, int offset, int size)
{
   const int base = regs_[reg] + offset;

   assert(base >= 0);
   assert(base + size <= MEM_SIZE);
   assert(base % InterpMachine::WORD_SIZE == 0);

#if DEBUG
   for (int i = 0; i < size; i += InterpMachine::WORD_SIZE)
      init_mask_.set((base + i) / InterpMachine::WORD_SIZE);
#endif

   return mem_[base / InterpMachine::WORD_SIZE];
}

const uint32_t& Interpreter::mem_rd(int reg, int offset, int size) const
{
   unsigned base = regs_[reg] + offset;

   if (base & 0x80000000) {
      base &= 0x7fffffff;

      assert(base + size <= bytecode_->data_length());
      return *((uint32_t *)bytecode_->data() + base);
   }
   else {
      assert(base + size <= MEM_SIZE);
      assert(base % InterpMachine::WORD_SIZE == 0);

#if DEBUG
      if (unlikely(init_mask_.is_clear(base / InterpMachine::WORD_SIZE)))
         fatal_trace("read uninitialised memory at 0x%04x", base);
#endif

      return mem_[base / InterpMachine::WORD_SIZE];
   }
}

void Interpreter::rtcall(Bytecode::RtCall func)
{
   switch (func) {
   case Bytecode::RT_REPORT:
      {
         rt_severity_t severity = (rt_severity_t)regs_[0];
         unsigned length = regs_[2];
         const char *message = (char *)&(mem_rd(1, 0, length));
         handler_->report(severity, message, length);
      }
      break;

   default:
      should_not_reach_here("unhandled rtcall %d", func);
   }
}

Interpreter::reg_t Interpreter::run(const Bytecode *code)
{
   WithCrashHandler handler(this);

   bytecode_ = code;
   bytes_ = code->code();
   bci_ = 0;
   last_bci_ = 0;

   int32_t a, b, c;
   for (;;) {
      last_bci_ = bci_;

      switch (opcode()) {
      case Bytecode::ADDB:
         a = reg();
         b = imm8();
         regs_[a] += b;
         break;

      case Bytecode::ADD:
         a = reg();
         b = reg();
         regs_[a] += regs_[b];
         break;

      case Bytecode::SUB:
         a = reg();
         b = reg();
         regs_[a] -= regs_[b];
         break;

      case Bytecode::RET:
         return regs_[0];

      case Bytecode::NOP:
         break;

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
         a = reg();
         b = imm16();
         c = reg();
         mem_wr(a, b) = regs_[c];
         break;

      case Bytecode::LDR:
         a = reg();
         b = reg();
         c = imm16();
         regs_[a] = mem_rd(b, c);
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

      case Bytecode::TESTB:
         a = reg();
         b = imm8();
         flags_ = interp_test(regs_[a], b);
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

      case Bytecode::MULB:
         a = reg();
         b = imm8();
         regs_[a] *= b;
         break;

      case Bytecode::ENTER:
         a = imm16();
         push(regs_[InterpMachine::FP_REG]);
         regs_[InterpMachine::FP_REG] = regs_[InterpMachine::SP_REG];
         assert(regs_[InterpMachine::SP_REG] - a >= 0);
         regs_[InterpMachine::SP_REG] -= a;
         break;

      case Bytecode::LEAVE:
         regs_[InterpMachine::SP_REG] = regs_[InterpMachine::FP_REG];
         regs_[InterpMachine::FP_REG] = pop();
         break;

      case Bytecode::RELDATA:
         a = reg();
         b = imm16();
         regs_[a] = 0x80000000 | b;
         break;

      case Bytecode::RTCALL:
         a = imm8();
         rtcall((Bytecode::RtCall)a);
         break;

      default:
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

void Interpreter::on_crash()
{
   bytecode_->dump(StdoutPrinter(), last_bci_);

   printf("\nRegisters:\n  ");
   for (int i = 0; i < InterpMachine::NUM_REGS; i++) {
      if (i == InterpMachine::SP_REG)
         printf("SP  ");
      else if (i == InterpMachine::FP_REG)
         printf("FP  ");
      else
         printf("R%-2d ", i);
      printf("%08x%s", regs_[i], i % 4 == 3 ? "\n  " : "    ");
   }

   printf("\nStack:\n  ");

   const int sp = regs_[InterpMachine::SP_REG];
   const int fp = regs_[InterpMachine::FP_REG];
   const int high = (std::min(fp + 16, STACK_SIZE - 4) + 3) & ~3;
   const int low = std::max(sp, 0) & ~3;

   int col = 0;
   for (int i = high; i >= low; i -= 4) {
      if (i == sp)
         printf("SP=> ");
      else if (i == fp)
         printf("FP=> ");
      else
         printf("     ");

      printf("%04x %08x%s", i, mem_[i / 4], col++ % 4 == 3 ? "\n  " : "  ");
   }

   if (col % 4 != 0)
      printf("\n");
}

void Interpreter::report(rt_severity_t severity, const char *message,
                         size_t length)
{
   color_printf("$bold$$green$%*s$$\n", (int)length, message);
}
