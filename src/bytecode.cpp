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
#include "common.h"

#include <vector>
#include <map>
#include <assert.h>
#include <string.h>
#include <ostream>

template class ArrayList<unsigned>;
template class ArrayList<uint8_t>;

namespace {
   class Dumper {
   public:
      Dumper(Printer& printer, const Bytecode *b, int mark_bci);

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
      void rtcall();

      const uint8_t  *bptr_;
      const Bytecode *bytecode_;
      Printer&        printer_;

      int col_ = 0, pos_ = 0;
      int mark_bci_;
   };
}

Dumper::Dumper(Printer& printer, const Bytecode *b, int mark_bci)
   : bptr_(b->code()),
     bytecode_(b),
     printer_(printer),
     mark_bci_(mark_bci)
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
   int offset = bytecode_->machine().read_i16(bptr_ + 1);

   if (offset == 0)
      col_ += printer_.print("%s[%s]", pos_ == 0 ? " " : ", ",
                             bytecode_->machine().fmt_reg(*bptr_));
   else
      col_ += printer_.print("%s[%s%+d]", pos_ == 0 ? " " : ", ",
                             bytecode_->machine().fmt_reg(*bptr_), offset);
   bptr_ += 3;
   pos_++;
}

void Dumper::immed32()
{
   assert(bptr_ + 4 <= bytecode_->code() + bytecode_->code_length());

   col_ += printer_.print("%s%d", pos_ == 0 ? " " : ", ",
                          bytecode_->machine().read_i32(bptr_));
   bptr_ += 4;
   pos_++;
}

void Dumper::immed16()
{
   assert(bptr_ + 2 <= bytecode_->code() + bytecode_->code_length());

   col_ += printer_.print("%s%d", pos_ == 0 ? " " : ", ",
                          bytecode_->machine().read_i16(bptr_));
   bptr_ += 2;
   pos_++;
}

void Dumper::immed8()
{
   int val = (int8_t)*bptr_;
   col_ += printer_.print("%s%d", pos_ == 0 ? " " : ", ", val);
   bptr_++;
   pos_++;
}

void Dumper::rtcall()
{
   const char *func = "???";
   switch ((Bytecode::RtCall)*bptr_) {
   case Bytecode::RT_IMAGE:      func = "image"; break;
   case Bytecode::RT_REPORT:     func = "report"; break;
   case Bytecode::RT_UARRAY_LEN: func = "uarray_len"; break;
   }

   col_ += printer_.print("%s%s", pos_ == 0 ? " " : ", ", func);
   bptr_++;
   pos_++;
}

void Dumper::jump_target()
{
   assert(bptr_ + 2 <= bytecode_->code() + bytecode_->code_length());

   const int delta = bytecode_->machine().read_i16(bptr_);

   col_ += printer_.print("%s%d", pos_ == 0 ? " " : ", ",
                          (int)(bptr_ - bytecode_->code() + delta));
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
   case Bytecode::SUB:
      opcode("SUB");
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
   case Bytecode::MULW:
      opcode("MULW");
      reg();
      immed32();
      break;
   case Bytecode::MULB:
      opcode("MULB");
      reg();
      immed8();
      break;
   case Bytecode::ANDB:
      opcode("ANDB");
      reg();
      immed8();
      break;
   case Bytecode::ANDW:
      opcode("ANDW");
      reg();
      immed32();
      break;
   case Bytecode::TESTB:
      opcode("TESTB");
      reg();
      immed8();
      break;
   case Bytecode::TESTW:
      opcode("TESTW");
      reg();
      immed32();
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
   case Bytecode::LEA:
      opcode("LEA");
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
   case Bytecode::JMPC:
      opcode("JMPC");
      condition();
      jump_target();
      break;
   case Bytecode::ENTER:
      opcode("ENTER");
      immed16();
      break;
   case Bytecode::LEAVE:
      opcode("LEAVE");
      break;
   case Bytecode::RELDATA:
      opcode("RELDATA");
      reg();
      immed16();
      break;
   case Bytecode::RTCALL:
      opcode("RTCALL");
      rtcall();
      break;
   default:
      fatal("invalid bytecode %02x", *bptr_);
   }
}

void Dumper::dump()
{
   printer_.print("CODE\n");

   while (bptr_ < bytecode_->code() + bytecode_->code_length()) {
      const uint8_t *startp = bptr_;
      col_ = 0;
      pos_ = 0;

      const int bci = bptr_ - bytecode_->code();

      if (bci == mark_bci_)
         printer_.color_print("$bold$$red$");

      col_ += printer_.print("%c%4d ", bci == mark_bci_ ? '*' : ' ', bci);

      diassemble_one();

      while (col_ < 30)
         col_ += printer_.print(" ");

      for (const uint8_t *p2 = startp; p2 < bptr_; p2++)
         col_ += printer_.print(" %02x", *p2);

#if DEBUG
      const char *comment = bytecode_->comment(startp);
      if (comment != nullptr) {
         while (col_ < 50)
            col_ += printer_.print(" ");

         printer_.print("; %s", comment);
      }
#endif

      if (bci == mark_bci_)
         printer_.color_print("$$");

      printer_.print("\n");

      assert(bptr_ > startp);
   }

   assert(bptr_ == bytecode_->code() + bytecode_->code_length());

   int data_len = bytecode_->data_length();
   if (data_len > 0) {
      printer_.print("DATA");

      const uint8_t *data = bytecode_->data();
      for (int i = 0; i < data_len; i += 16) {
         printer_.print("\n ");

         for (int j = 0; j < 16; j++) {
            if (i + j < data_len)
               printer_.print(" %02x", data[i + j]);
            else
               printer_.print("   ");
         }

         printer_.print(" |");

         for (int j = 0; j < 16; j++) {
            if (i + j < data_len) {
               char ch = isalnum((int)data[i + j]) ? data[i + j] : '.';
               printer_.print("%c", ch);
            }
            else
               printer_.print(" ");
         }

         printer_.print("|\n");
      }

      printer_.print("\n");
   }
}

Bytecode::Bytecode(const Machine& m, const uint8_t *code, size_t code_len,
                   const uint8_t *data, size_t data_len)
   : code_(new uint8_t[code_len]),
     code_len_(code_len),
     data_(new uint8_t[data_len]),
     data_len_(data_len),
     machine_(m)
{
   memcpy(code_, code, code_len);
   memcpy(data_, data, data_len);
}

Bytecode::~Bytecode()
{
   delete[] code_;
   delete[] data_;

#if DEBUG
   for (auto &p : comments_)
      free(p.second);
#endif
}

void Bytecode::dump(Printer&& printer, int mark_bci) const
{
   Dumper(printer, this, mark_bci).dump();
}

void Bytecode::dump(Printer& printer, int mark_bci) const
{
   Dumper(printer, this, mark_bci).dump();
}

#if DEBUG
void Bytecode::move_comments(std::map<int, char*>&& comments)
{
   comments_ = comments;
}

const char *Bytecode::comment(const uint8_t *bptr) const
{
   const int offset = bptr - code_;
   auto it = comments_.find(offset);
   if (it == comments_.end())
      return nullptr;
   else
      return it->second;
}
#endif  // DEBUG

Bytecode::Assembler::Assembler(const Machine& m)
   : machine_(m)
{

}

void Bytecode::Assembler::comment(const char *fmt, ...)
{
#if DEBUG
   va_list ap;
   va_start(ap, fmt);
   char *buf = xvasprintf(fmt, ap);
   va_end(ap);

   const int offset = code_.size();
   auto it = comments_.find(offset);
   if (it == comments_.end())
      comments_[offset] = buf;
   else {
      buf[0] = tolower(buf[0]);
      comments_[offset] = xasprintf("%s, %s", it->second, buf);
      free(buf);
   }
#endif
}

void Bytecode::Assembler::mov(Register dst, Register src)
{
   if (dst != src) {
      emit_u8(Bytecode::MOV);
      emit_reg(dst);
      emit_reg(src);
   }
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

void Bytecode::Assembler::jmp(Label& target)
{
   const unsigned start = code_.size();
   emit_u8(Bytecode::JMP);
   emit_branch(start, target);
}

void Bytecode::Assembler::jmp(Label& target, Condition cond)
{
   const unsigned start = code_.size();
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

void Bytecode::Assembler::lea(Register dst, Register indirect, int16_t offset)
{
   emit_u8(Bytecode::LEA);
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
   if (is_int8(value)) {
      emit_u8(Bytecode::MOVB);
      emit_reg(dst);
      emit_u8(value);
   }
   else if (is_int32(value)) {
      emit_u8(Bytecode::MOVW);
      emit_reg(dst);
      emit_i32(value);
   }
   else
      should_not_reach_here("64-bit immediate");
}

void Bytecode::Assembler::add(Register dst, int64_t value)
{
   if (is_int8(value)) {
      emit_u8(Bytecode::ADDB);
      emit_reg(dst);
      emit_u8(value);
   }
   else if (is_int32(value)) {
     emit_u8(Bytecode::ADDW);
     emit_reg(dst);
     emit_i32(value);
   }
   else
      should_not_reach_here("64-bit immediate");
}

void Bytecode::Assembler::andr(Register dst, int64_t value)
{
   if (is_int8(value)) {
      emit_u8(Bytecode::ANDB);
      emit_reg(dst);
      emit_u8(value);
   }
   else if (is_int32(value)) {
      emit_u8(Bytecode::ANDW);
      emit_reg(dst);
      emit_i32(value);
   }
   else
      should_not_reach_here("64-bit immediate");
}

void Bytecode::Assembler::test(Register dst, int64_t value)
{
   if (is_int8(value)) {
      emit_u8(Bytecode::TESTB);
      emit_reg(dst);
      emit_u8(value);
   }
   else if (is_int32(value)) {
      emit_u8(Bytecode::TESTW);
      emit_reg(dst);
      emit_i32(value);
   }
   else
      should_not_reach_here("64-bit immediate");
}

void Bytecode::Assembler::sub(Register dst, Register src)
{
   emit_u8(Bytecode::SUB);
   emit_reg(dst);
   emit_reg(src);
}

void Bytecode::Assembler::add(Register dst, Register src)
{
   emit_u8(Bytecode::ADD);
   emit_reg(dst);
   emit_reg(src);
}

void Bytecode::Assembler::mul(Register dst, Register rhs)
{
   emit_u8(Bytecode::MUL);
   emit_reg(dst);
   emit_reg(rhs);
}

void Bytecode::Assembler::mul(Register dst, int64_t value)
{
   // TODO: shift if power of 2
   if (is_int8(value)) {
      emit_u8(Bytecode::MULB);
      emit_reg(dst);
      emit_u8(value);
   }
   else if (is_int32(value)) {
      emit_u8(Bytecode::MULW);
      emit_reg(dst);
      emit_i32(value);
   }
   else
      should_not_reach_here("64-bit immediate");
}

void Bytecode::Assembler::data(const uint8_t *bytes, size_t len)
{
   for (size_t i = 0; i < len; i++)
      data_.add(bytes[i]);
}

void Bytecode::Assembler::enter(uint16_t frame_size)
{
   assert(frame_size % machine_.stack_align() == 0);
   emit_u8(Bytecode::ENTER);
   emit_i16(frame_size);
}

void Bytecode::Assembler::reldata(Register dst, uint16_t offset)
{
   assert(offset < data_.size());
   emit_u8(Bytecode::RELDATA);
   emit_reg(dst);
   emit_i16(offset);
}

void Bytecode::Assembler::rtcall(RtCall func)
{
   emit_u8(Bytecode::RTCALL);
   emit_u8(func);
}

void Bytecode::Assembler::leave()
{
   emit_u8(Bytecode::LEAVE);
}

void Bytecode::Assembler::emit_reg(Register reg)
{
   assert(machine_.num_regs() <= 256);
   assert(reg.num < 256);
   emit_u8(reg.num);
}

void Bytecode::Assembler::emit_u8(uint8_t byte)
{
   code_.add(byte);
}

void Bytecode::Assembler::emit_i32(int32_t value)
{
   code_.add(value & 0xff);
   code_.add((value >> 8) & 0xff);
   code_.add((value >> 16) & 0xff);
   code_.add((value >> 24) & 0xff);
}

void Bytecode::Assembler::emit_i16(int16_t value)
{
   code_.add(value & 0xff);
   code_.add((value >> 8) & 0xff);
}

void Bytecode::Assembler::bind(Label &label)
{
   label.bind(this, code_.size());
}

void Bytecode::Assembler::patch_branch(unsigned offset, unsigned abs)
{
   switch (code_[offset]) {
   case Bytecode::JMP:  offset += 1; break;
   case Bytecode::JMPC: offset += 2; break;
   default:
      should_not_reach_here("cannot patch %02x", code_[offset]);
   }

   assert(offset + 2 <= code_.size());

   const int delta = abs - offset;

   code_[offset] = delta & 0xff;
   code_[offset + 1] = (delta >> 8) & 0xff;
}

void Bytecode::Assembler::emit_branch(unsigned offset, Label& target)
{
   if (target.bound())
      emit_i16(target.target() - code_.size());
   else {
      target.add_patch(offset);
      emit_i16(-1);
   }
}

Bytecode *Bytecode::Assembler::finish()
{
   Bytecode *b = new Bytecode(machine_, code_.data(), code_.size(),
                              data_.data(), data_.size());
   DEBUG_ONLY(b->move_comments(std::move(comments_)));
   return b;
}

Bytecode::Label::~Label()
{
   assert(patch_list_.size() == 0);
}

void Bytecode::Label::add_patch(unsigned offset)
{
   patch_list_.add(offset);
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

Machine::Machine(const char *name, const Desc& desc)
   : name_(name),
     desc_(desc)
{
}

const char *Machine::fmt_reg(int reg) const
{
   assert(reg < desc_.num_regs);

   if (reg == desc_.sp_reg)
      return "SP";
   else if (reg == desc_.fp_reg)
      return "FP";
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
   : Machine("interp",
             Desc {
                .num_regs = NUM_REGS,
                .result_reg = 0,
                .sp_reg = SP_REG,
                .fp_reg = FP_REG,
                .word_size = WORD_SIZE,
                .stack_align = WORD_SIZE,
                .frame_reserved = 4,   // Saved FP
             })
{
}

const InterpMachine& InterpMachine::get()
{
   static InterpMachine m;
   return m;
}
