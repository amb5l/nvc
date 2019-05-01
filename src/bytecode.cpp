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

#include <vector>
#include <map>
#include <assert.h>
#include <string.h>
#include <ostream>

#define __ asm_.

namespace {
   class Compiler {
   public:
      explicit Compiler(const Machine& m);
      Compiler(const Compiler&) = delete;

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

      Bytecode::Label& label_for_block(vcode_block_t block);

      struct Mapping {
         enum Kind { REGISTER, STACK };

         Kind kind;
         union {
            Bytecode::Register reg;
            int slot;
         };
      };

      struct Patch {
         vcode_block_t   block;
         Bytecode::Label label;
      };

      const Mapping& map_vcode_reg(vcode_reg_t reg) const;
      const Mapping& map_vcode_var(vcode_reg_t reg) const;

      const Machine                  machine_;
      Bytecode::Assembler            asm_;
      std::map<vcode_var_t, Mapping> var_map_;
      std::vector<Mapping>           reg_map_;
      std::vector<Bytecode::Label>   block_map_;
   };

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
