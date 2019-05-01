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
#include "vcode.h"
#include "printer.hpp"

#include <iosfwd>
#include <vector>

class Machine {
public:
   Machine(const char *name, int num_regs, int result_reg, int sp_reg);
   Machine(const Machine&) = default;
   Machine(Machine&&) = default;
   virtual ~Machine() {}

   const char *name() const { return name_; }
   int num_regs() const { return num_regs_; }
   int result_reg() const { return result_reg_; }
   int sp_reg() const { return sp_reg_; }

   int32_t read_i32(const uint8_t *p) const;
   int16_t read_i16(const uint8_t *p) const;

   virtual const char *fmt_reg(int reg) const;

private:
   const char *const name_;
   const int         num_regs_;
   const int         result_reg_;
   const int         sp_reg_;
};

class InterpMachine : public Machine {
public:
   static const InterpMachine& get();

   static const int NUM_REGS = 256;

private:
   InterpMachine();
};

class Bytecode {
public:
   enum OpCode : uint8_t {
      NOP  = 0x00,     // Do nothing
      MOVW = 0x01,     // Move 32-bit literal to register
      RET  = 0x02,     // Return from function
      ADD  = 0x03,     // Add two registers
      MOV  = 0x04,     // Move register to another register
      ADDW = 0x05,     // Add 32-bit immediate to register
      STR  = 0x06,     // Store register to memory (indirect)
      LDR  = 0x07,     // Load register from memory (indirect)
      MUL  = 0x08,     // Multiply 32-bit registers
      CMP  = 0x09,     // Compare two registers
      CSET = 0x0a,     // Set register based on flags
      JMP  = 0x0b,     // Jump to address
      CBZ  = 0x0c,     // Compare and branch if zero
      CBNZ = 0x0d,     // Compare and branch if non-zero
      MOVB = 0x0e,     // Move 8-bit literal to register
      ADDB = 0x0f,     // Add 8-bit immediate to register
      JMPC = 0x10,     // Jump if condition code set
   };

   enum Condition : uint8_t {
      Z = 0x01, NZ = 0x02, GT = 0x04, LT = 0x08, GE = 0x10, LE = 0x20,
      EQ = Z, NE = NZ
   };

   struct Register {
      int num;
   };

   static inline Register R(int num) {
      return Register { num };
   }

   class Assembler;

   class Label {
   public:
      Label() = default;
      Label(const Label&) = delete;
      Label(Label&&) = default;
      ~Label();

      void bind(Assembler *owner, unsigned target);
      void add_patch(unsigned offset);
      bool bound() const { return bound_ != -1; }
      unsigned target() const;

   private:
      int bound_ = -1;
      std::vector<unsigned> patch_list_;
   };

   class Assembler {
   public:
      explicit Assembler(const Machine& m);

      Bytecode *finish();
      void set_frame_size(unsigned bytes);
      unsigned code_size() const { return bytes_.size(); }
      void patch_branch(unsigned offset, unsigned abs);
      void bind(Label& label);

      void mov(Register dst, Register src);
      void mov(Register dst, int64_t value);
      void add(Register dst, Register src);
      void add(Register dst, int64_t value);
      void str(Register indirect, int16_t offset, Register src);
      void ldr(Register dst, Register indirect, int16_t offset);
      void ret();
      void cmp(Register lhs, Register rhs);
      void cset(Register dst, Condition cond);
      void cbz(Register src, Label& label);
      void cbnz(Register src, Label& label);
      void jmp(Label& label);
      void jmp(Label& target, Condition cond);
      void mul(Register dst, Register rhs);
      void nop();

      Register sp() const { return Register{ machine_.sp_reg() }; };

      private:
        void emit_reg(Register reg);
        void emit_u8(uint8_t byte);
        void emit_i32(int32_t value);
        void emit_i16(int16_t value);
        void emit_branch(unsigned offset, Label& target);

        Assembler(const Assembler &) = delete;
        Assembler(Assembler &&) = default;

        std::vector<uint8_t> bytes_;
        const Machine machine_;
        unsigned frame_size_ = 0;
   };

   static Bytecode *compile(const Machine& m, vcode_unit_t unit);

   const uint8_t *bytes() const { return bytes_; }
   size_t length() const { return len_; }
   const Machine& machine() const { return machine_; }
   unsigned frame_size() const { return frame_size_; }

   void dump(Printer&& printer = StdoutPrinter()) const;
   void dump(Printer& printer) const;

private:
   explicit Bytecode(const Machine& m, const uint8_t *bytes, size_t len,
                     unsigned frame_size);
   Bytecode(const Bytecode&) = delete;
   Bytecode(const Bytecode&&) = delete;
   ~Bytecode();

   uint8_t *const  bytes_;
   const size_t    len_;
   const unsigned  frame_size_;
   const Machine   machine_;
};

std::ostream& operator<<(std::ostream& os, const Bytecode& b);
