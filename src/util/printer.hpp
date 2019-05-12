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

#include <cstdio>

class Printer {
public:
   virtual ~Printer() {}

   __attribute__((format(printf, 2, 3)))
   virtual int print(const char *fmt, ...);

   __attribute__((format(printf, 2, 3)))
   virtual int color_print(const char *fmt, ...);

   virtual int color_vprint(const char *fmt, va_list ap);
   virtual int vprint(const char *fmt, va_list ap) = 0;
   virtual void flush() {}
   virtual void copy(const char *str);
   virtual void copy(char ch);

protected:
   static void filter_color(const char *str, Printer& out, bool want_color);
   static bool has_color_escape(const char *str);
};

class FilePrinter : public Printer {
public:
   explicit FilePrinter(FILE *f) : file_(f) {}

   int vprint(const char *fmt, va_list ap) override;
   void flush() override;
   void copy(const char *str) override;
   void copy(char ch) override;

private:
   FILE *file_;
};

class TerminalPrinter : public FilePrinter {
public:
   explicit TerminalPrinter(FILE *f, bool force_color=false);

   int color_vprint(const char *fmt, va_list ap) override;

private:
   static bool detect_terminal(int fno);

   bool want_color_;
};

class StdoutPrinter : public TerminalPrinter {
public:
   StdoutPrinter() : TerminalPrinter(stdout) {}
};

class BufferPrinter : public Printer {
public:
   BufferPrinter();
   ~BufferPrinter();
   BufferPrinter(const BufferPrinter&) = delete;

   int vprint(const char *fmt, va_list ap) override;
   void copy(const char *str) override;
   void copy(char ch) override;

   const char *buffer() const { return buffer_; }
   void clear();

private:
   static const int DEFAULT_BUFFER = 256;

   void grow(size_t nchars);

   char *buffer_;
   char *wptr_;
   size_t len_;
};
