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
#include "compiler.hpp"

#include <vector>
#include <map>
#include <assert.h>
#include <string.h>
#include <ostream>

namespace {
   class Dumper {
   public:
      Dumper(Printer& printer, const Bytecode *b);

      void dump();

   private:
      void diassemble_one();
      void opcode(const char *name);
      void reg();
      void immed32();
      void immed16();
      void immed8();
      void indirect();
      void jump_target();
      void condition();

      const uint8_t  *bptr_;
      const Bytecode *bytecode_;
      Printer&        printer_;

      int col_ = 0, pos_ = 0;
   };
}

Dumper::Dumper(Printer& printer, const Bytecode *b)
   : bptr_(b->bytes()),
     bytecode_(b),
     printer_(printer)
{
}

void Dumper::opcode(const char *name)
{
   col_ += printer_.print("%s", name);
   bptr_++;
}

void Dumper::reg()
{
   col_ += printer_.print("%s%s", pos_ == 0 ? " " : ", ",
                          bytecode_->machine().fmt_reg(*bptr_));
   bptr_++;
   pos_++;
}

void Dumper::condition()
{
   const char *names[] = { "Z", "NZ", "GT", "LT", "GE", "LE" };
   assert(*bptr_ < ARRAY_LEN(names));

   const char *name = "?";
   switch ((Bytecode::Condition)*bptr_) {
   case Bytecode::Z:  name = "Z";  break;
   case Bytecode::NZ: name = "NZ"; break;
   case Bytecode::GT: name = "GT"; break;
   case Bytecode::LT: name = "LT"; break;
   case Bytecode::GE: name = "GE"; break;
   case Bytecode::LE: name = "LE"; break;
   }

   col_ += printer_.print("%s%s", pos_ == 0 ? " " : ", ", name);
   bptr_++;
   pos_++;
}

void Dumper::indirect()
{
   col_ += printer_.print("%s[%s%+d]", pos_ == 0 ? " " : ", ",
                          bytecode_->machine().fmt_reg(*bptr_),
                          bytecode_->machine().read_i16(bptr_ + 1));
   bptr_ += 3;
   pos_++;
}

void Dumper::immed32()
{
   assert(bptr_ + 4 <= bytecode_->bytes() + bytecode_->length());

   col_ += printer_.print("%s%d", pos_ == 0 ? " " : ", ",
                          bytecode_->machine().read_i32(bptr_));
   bptr_ += 4;
   pos_++;
}

void Dumper::immed16()
{
   assert(bptr_ + 2 <= bytecode_->bytes() + bytecode_->length());

   col_ += printer_.print("%s%d", pos_ == 0 ? " " : ", ",
                          bytecode_->machine().read_i16(bptr_));
   bptr_ += 2;
   pos_++;
}

void Dumper::immed8()
{
   col_ += printer_.print("%s%d", pos_ == 0 ? " " : ", ", *bptr_);
   bptr_++;
   pos_++;
}

void Dumper::jump_target()
{
   assert(bptr_ + 2 <= bytecode_->bytes() + bytecode_->length());

   const int delta = bytecode_->machine().read_i16(bptr_);

   col_ += printer_.print("%s%d", pos_ == 0 ? " " : ", ",
                          (int)(bptr_ - bytecode_->bytes() + delta));
   bptr_ += 2;
   pos_++;
}

void Dumper::diassemble_one()
{
   switch ((Bytecode::OpCode)*bptr_) {
   case Bytecode::NOP:
      opcode("NOP");
      break;
   case Bytecode::MOVW:
      opcode("MOVW");
      reg();
      immed32();
      break;
   case Bytecode::MOVB:
      opcode("MOVB");
      reg();
      immed8();
      break;
   case Bytecode::RET:
      opcode("RET");
      break;
   case Bytecode::ADD:
      opcode("ADD");
      reg();
      reg();
      break;
   case Bytecode::MOV:
      opcode("MOV");
      reg();
      reg();
      break;
   case Bytecode::ADDW:
      opcode("ADDW");
      reg();
      immed32();
      break;
   case Bytecode::ADDB:
      opcode("ADDB");
      reg();
      immed8();
      break;
   case Bytecode::STR:
      opcode("STR");
      indirect();
      reg();
      break;
   case Bytecode::LDR:
      opcode("LDR");
      reg();
      indirect();
      break;
   case Bytecode::MUL:
      opcode("MUL");
      reg();
      reg();
      break;
   case Bytecode::CSET:
      opcode("CSET");
      reg();
      condition();
      break;
   case Bytecode::CMP:
      opcode("CMP");
      reg();
      reg();
      break;
   case Bytecode::JMP:
      opcode("JMP");
      jump_target();
      break;
   case Bytecode::CBZ:
      opcode("CBZ");
      reg();
      jump_target();
      break;
   case Bytecode::CBNZ:
      opcode("CBNZ");
      reg();
      jump_target();
      break;
   case Bytecode::JMPC:
      opcode("JMPC");
      condition();
      jump_target();
      break;
   default:
      fatal("invalid bytecode %02x", *bptr_);
   }
}

void Dumper::dump()
{
   if (bytecode_->frame_size() > 0)
      printer_.print("FRAME %d BYTES\n", bytecode_->frame_size());

   printer_.print("CODE\n");

   while (bptr_ < bytecode_->bytes() + bytecode_->length()) {
      const uint8_t *startp = bptr_;
      col_ = 0;
      pos_ = 0;

      col_ += printer_.print("%4d ", (int)(bptr_ - bytecode_->bytes()));

      diassemble_one();

      while (col_ < 30)
         col_ += printer_.print(" ");

      for (const uint8_t *p2 = startp; p2 < bptr_; p2++)
         printer_.print(" %02x", *p2);

      printer_.print("\n");

      assert(bptr_ > startp);
   }

   assert(bptr_ == bytecode_->bytes() + bytecode_->length());
}

Bytecode::Bytecode(const Machine& m, const uint8_t *bytes, size_t len,
                   unsigned frame_size)
   : bytes_(new uint8_t[len]),
     len_(len),
     frame_size_(frame_size),
     machine_(m)
{

   memcpy(bytes_, bytes, len);
}

Bytecode::~Bytecode()
{
   delete bytes_;
}

Bytecode *Bytecode::compile(const Machine& m, vcode_unit_t unit)
{
   return Compiler(m).compile(unit);
}

void Bytecode::dump(Printer&& printer) const
{
   Dumper(printer, this).dump();
}

void Bytecode::dump(Printer& printer) const
{
   Dumper(printer, this).dump();
}

Bytecode::Assembler::Assembler(const Machine& m)
   : machine_(m)
{

}

void Bytecode::Assembler::mov(Register dst, Register src)
{
   emit_u8(Bytecode::MOV);
   emit_reg(dst);
   emit_reg(src);
}

void Bytecode::Assembler::cmp(Register lhs, Register rhs)
{
   emit_u8(Bytecode::CMP);
   emit_reg(lhs);
   emit_reg(rhs);
}

void Bytecode::Assembler::cset(Register dst, Condition cond)
{
   emit_u8(Bytecode::CSET);
   emit_reg(dst);
   emit_u8(cond);
}

void Bytecode::Assembler::cbnz(Register src, Label& target)
{
   const unsigned start = bytes_.size();
   emit_u8(Bytecode::CBNZ);
   emit_reg(src);
   emit_branch(start, target);
}

void Bytecode::Assembler::cbz(Register src, Label& target)
{
   const unsigned start = bytes_.size();
   emit_u8(Bytecode::CBZ);
   emit_reg(src);
   emit_branch(start, target);
}

void Bytecode::Assembler::jmp(Label& target)
{
   const unsigned start = bytes_.size();
   emit_u8(Bytecode::JMP);
   emit_branch(start, target);
}

void Bytecode::Assembler::jmp(Label& target, Condition cond)
{
   const unsigned start = bytes_.size();
   emit_u8(Bytecode::JMPC);
   emit_u8(cond);
   emit_branch(start, target);
}

void Bytecode::Assembler::str(Register indirect, int16_t offset, Register src)
{
   emit_u8(Bytecode::STR);
   emit_reg(indirect);
   emit_i16(offset);
   emit_reg(src);
}

void Bytecode::Assembler::ldr(Register dst, Register indirect, int16_t offset)
{
   emit_u8(Bytecode::LDR);
   emit_reg(dst);
   emit_reg(indirect);
   emit_i16(offset);
}

void Bytecode::Assembler::ret()
{
   emit_u8(Bytecode::RET);
}

void Bytecode::Assembler::nop()
{
   emit_u8(Bytecode::NOP);
}

void Bytecode::Assembler::mov(Register dst, int64_t value)
{
   if (value >= INT8_MIN && value <= INT8_MAX) {
      emit_u8(Bytecode::MOVB);
      emit_reg(dst);
      emit_u8(value);
   }
   else {
      emit_u8(Bytecode::MOVW);
      emit_reg(dst);
      emit_i32(value);
   }
}

void Bytecode::Assembler::add(Register dst, int64_t value)
{
   if (value >= INT8_MIN && value <= INT8_MAX) {
      emit_u8(Bytecode::ADDB);
      emit_reg(dst);
      emit_u8(value);
   } else {
     emit_u8(Bytecode::ADDW);
     emit_reg(dst);
     emit_i32(value);
   }
}

void Bytecode::Assembler::mul(Register dst, Register rhs)
{
   emit_u8(Bytecode::MUL);
   emit_reg(dst);
   emit_reg(rhs);
}

void Bytecode::Assembler::emit_reg(Register reg)
{
   assert(machine_.num_regs() <= 256);
   assert(reg.num < 256);
   emit_u8(reg.num);
}

void Bytecode::Assembler::emit_u8(uint8_t byte)
{
   bytes_.push_back(byte);
}

void Bytecode::Assembler::emit_i32(int32_t value)
{
   bytes_.push_back(value & 0xff);
   bytes_.push_back((value >> 8) & 0xff);
   bytes_.push_back((value >> 16) & 0xff);
   bytes_.push_back((value >> 24) & 0xff);
}

void Bytecode::Assembler::emit_i16(int16_t value)
{
   bytes_.push_back(value & 0xff);
   bytes_.push_back((value >> 8) & 0xff);
}

void Bytecode::Assembler::bind(Label &label)
{
   label.bind(this, bytes_.size());
}

void Bytecode::Assembler::patch_branch(unsigned offset, unsigned abs)
{
   switch (bytes_[offset]) {
   case Bytecode::JMP:  offset += 1; break;
   case Bytecode::JMPC:
   case Bytecode::CBZ:
   case Bytecode::CBNZ: offset += 2; break;
   default:
      should_not_reach_here("cannot patch %02x", bytes_[offset]);
   }

   assert(offset + 2 <= bytes_.size());

   const int delta = abs - offset;

   bytes_[offset] = delta & 0xff;
   bytes_[offset + 1] = (delta >> 8) & 0xff;
}

void Bytecode::Assembler::emit_branch(unsigned offset, Label& target)
{
   if (target.bound())
      emit_i16(target.target() - bytes_.size());
   else {
      target.add_patch(offset);
      emit_i16(-1);
   }
}

void Bytecode::Assembler::set_frame_size(unsigned size)
{
   frame_size_ = size;
}

Bytecode *Bytecode::Assembler::finish()
{
   return new Bytecode(machine_, bytes_.data(), bytes_.size(), frame_size_);
}

Bytecode::Label::~Label()
{
   assert(patch_list_.size() == 0);
}

void Bytecode::Label::add_patch(unsigned offset)
{
   patch_list_.push_back(offset);
}

unsigned Bytecode::Label::target() const
{
   assert(bound_ >= 0);
   return bound_;
}

void Bytecode::Label::bind(Assembler *owner, unsigned target)
{
   assert(bound_ == -1);

   for (unsigned patch : patch_list_) {
      owner->patch_branch(patch, target);
   }

   bound_ = target;
   patch_list_.clear();
}

Machine::Machine(const char *name, int num_regs, int result_reg, int sp_reg)
   : name_(name),
     num_regs_(num_regs),
     result_reg_(result_reg),
     sp_reg_(sp_reg)
{
}

const char *Machine::fmt_reg(int reg) const
{
   assert(reg < num_regs_);

   if (reg == sp_reg_)
      return "SP";
   else {
      static char buf[32]; // XXX
      checked_sprintf(buf, sizeof(buf), "R%d", reg);
      return buf;
   }
}

int32_t Machine::read_i32(const uint8_t *p) const
{
   return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

int16_t Machine::read_i16(const uint8_t *p) const
{
   return p[0] | (p[1] << 8);
}

InterpMachine::InterpMachine()
   : Machine("interp", NUM_REGS, 0, 255)
{
}

const InterpMachine& InterpMachine::get()
{
   static InterpMachine m;
   return m;
}

std::ostream& operator<<(std::ostream& os, const Bytecode& b)
{
   BufferPrinter printer;
   b.dump(printer);
   return os << printer.buffer();
}
