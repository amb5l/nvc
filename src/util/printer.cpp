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

#include "printer.hpp"

#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cassert>
#include <cstring>
#include <unistd.h>

#define ANSI_RESET      0
#define ANSI_BOLD       1
#define ANSI_FG_BLACK   30
#define ANSI_FG_RED     31
#define ANSI_FG_GREEN   32
#define ANSI_FG_YELLOW  33
#define ANSI_FG_BLUE    34
#define ANSI_FG_MAGENTA 35
#define ANSI_FG_CYAN    36
#define ANSI_FG_WHITE   37

struct color_escape {
   const char *name;
   int         value;
};

static const struct color_escape escapes[] = {
   { "",        ANSI_RESET },
   { "bold",    ANSI_BOLD },
   { "black",   ANSI_FG_BLACK },
   { "red",     ANSI_FG_RED },
   { "green",   ANSI_FG_GREEN },
   { "yellow",  ANSI_FG_YELLOW },
   { "blue",    ANSI_FG_BLUE },
   { "magenta", ANSI_FG_MAGENTA },
   { "cyan",    ANSI_FG_CYAN },
   { "white",   ANSI_FG_WHITE },
};

bool Printer::has_color_escape(const char *str)
{
   return strchr(str, '$') != nullptr;
}

void Printer::filter_color(const char *str, Printer& out, bool want_color)
{
   // Replace color strings like "$red$foo$$bar" with ANSI escaped
   // strings like "\033[31mfoo\033[0mbar"

   const char *escape_start = nullptr;

   while (*str != '\0') {
      if (*str == '$') {
         if (escape_start == nullptr)
            escape_start = str;
         else {
            const char *e = escape_start + 1;
            const size_t len = str - e;

            if (want_color) {
               bool found = false;
               for (size_t i = 0; i < ARRAY_LEN(escapes); i++) {
                  if (strncmp(e, escapes[i].name, len) == 0) {
                     out.print("\033[%dm", escapes[i].value);
                     found = true;
                     break;
                  }
               }

               if (!found) {
                  out.append(escape_start);
                  escape_start = str;
               }
               else
                  escape_start = nullptr;
            }
            else
               escape_start = nullptr;
         }
      }
      else if (escape_start == nullptr)
         out.append(*str);

      ++str;
   }

   if (escape_start != nullptr)
      out.append(escape_start);
}

void Printer::append(const char *str)
{
   print("%s", str);
}

void Printer::append(const char *str, size_t len)
{
   print("%*s", (int)len, str);
}

void Printer::append(char ch)
{
   print("%c", ch);
}

int Printer::print(const char *fmt, ...)
{
   std::va_list ap;
   va_start(ap, fmt);
   int nchars = vprint(fmt, ap);
   va_end(ap);
   return nchars;
}

int Printer::color_print(const char *fmt, ...)
{
   std::va_list ap;
   va_start(ap, fmt);
   int nchars = color_vprint(fmt, ap);
   va_end(ap);
   return nchars;
}

int Printer::color_vprint(const char *fmt, va_list ap)
{
   if (has_color_escape(fmt)) {
      BufferPrinter filtered_fmt;
      filter_color(fmt, filtered_fmt, false);
      return vprint(filtered_fmt.buffer(), ap);
   }
   else
      return vprint(fmt, ap);
}

int FilePrinter::vprint(const char *fmt, va_list ap)
{
   return vfprintf(file_, fmt, ap);
}

void FilePrinter::flush()
{
   fflush(file_);
}

void FilePrinter::append(const char *str)
{
   fputs(str, file_);
}

void FilePrinter::append(const char *str, size_t len)
{
   fwrite(str, 1, len, file_);
}

void FilePrinter::append(char ch)
{
   fputc(ch, file_);
}

TerminalPrinter::TerminalPrinter(FILE *f, bool force_color)
   : FilePrinter(f)
{
   int fno = fileno(f);

   if (force_color)
      want_color_ = true;
   else if (fno == STDOUT_FILENO || fno == STDERR_FILENO) {
      want_color_ = detect_terminal(fno);
   }
   else
      want_color_ = false;
}

bool TerminalPrinter::detect_terminal(int fno)
{
   static int saved[STDERR_FILENO] = { -1, -1 };
   assert(fno >= 0 && fno <= STDERR_FILENO);

   if (saved[fno] != -1)
      return saved[fno];

   const char *nvc_no_color = getenv("NVC_NO_COLOR");
   const char *term = getenv("TERM");

   static const char *term_blacklist[] = {
      "dumb"
   };

   bool is_tty = isatty(fno);

#ifdef __MINGW32__
   if (!is_tty) {
      // Handle running under MinTty
      HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
      const size_t size = sizeof(FILE_NAME_INFO) + sizeof(WCHAR) * MAX_PATH;
      FILE_NAME_INFO *nameinfo = malloc(size);
      if (!GetFileInformationByHandleEx(hStdOut, FileNameInfo, nameinfo, size))
         fatal_errno("GetFileInformationByHandle");

      if ((wcsncmp(nameinfo->FileName, L"\\msys-", 6) == 0
           || wcsncmp(nameinfo->FileName, L"\\cygwin-", 8) == 0)
          && wcsstr(nameinfo->FileName, L"pty") != nullptr)
         is_tty = true;

      free(nameinfo);
   }
#endif

   bool want_color = is_tty && (nvc_no_color == nullptr);

   if (want_color && (term != nullptr)) {
      for (size_t i = 0; i < ARRAY_LEN(term_blacklist); i++) {
         if (strcmp(term, term_blacklist[i]) == 0) {
            want_color = false;
            break;
         }
      }
   }

#ifdef __MINGW32__
   HANDLE hConsole = GetStdHandle(STD_ERROR_HANDLE);
   DWORD mode;
   if (GetConsoleMode(hConsole, &mode)) {
      mode |= 0x04; // ENABLE_VIRTUAL_TERMINAL_PROCESSING
      if (!SetConsoleMode(hConsole, mode))
         want_color = false;
   }
#endif

   return (saved[fno] = want_color);
}

int TerminalPrinter::color_vprint(const char *fmt, va_list ap)
{
   if (has_color_escape(fmt)) {
      BufferPrinter filtered_fmt;
      filter_color(fmt, filtered_fmt, want_color_);
      return vprint(filtered_fmt.buffer(), ap);
   }
   else
      return vprint(fmt, ap);
}

BufferPrinter::BufferPrinter(size_t init_size)
   : buffer_((char *)xmalloc(init_size)),
     wptr_(buffer_),
     len_(init_size)
{
   buffer_[0] = '\0';
}

BufferPrinter::~BufferPrinter()
{
   buffer_[0] = '\0';
   free(buffer_);
}

int BufferPrinter::vprint(const char *fmt, va_list ap)
{
   va_list ap_copy;
   va_copy(ap_copy, ap);

   int nchars = vsnprintf(wptr_, buffer_ + len_ - wptr_, fmt, ap);

   if (wptr_ + nchars + 1 > buffer_ + len_) {
      grow(nchars + 1);
      vsnprintf(wptr_, buffer_ + len_ - wptr_, fmt, ap_copy);
   }

   va_end(ap_copy);

   wptr_ += nchars;
   return nchars;
}

void BufferPrinter::grow(size_t nchars)
{
   const size_t offset = wptr_ - buffer_;
   const size_t nlen = MAX(offset + nchars, len_ * 3);

   buffer_ = (char *)xrealloc(buffer_, nlen);
   len_ = nlen;
   wptr_ = buffer_ + offset;
}

void BufferPrinter::clear()
{
   wptr_ = buffer_;
   buffer_[0] = '\0';
}

void BufferPrinter::append(const char *str)
{
   const int nchars = strlen(str);
   if (wptr_ + nchars + 1 > buffer_ + len_)
      grow(nchars + 1);

   memcpy(wptr_, str, nchars + 1);
   wptr_ += nchars;
}

void BufferPrinter::append(const char *str, size_t len)
{
   if (wptr_ + len + 1 > buffer_ + len_)
      grow(len + 1);

   memcpy(wptr_, str, len);
   wptr_[len] = '\0';
   wptr_ += len;
}

void BufferPrinter::append(char ch)
{
   if (wptr_ + 2 > buffer_ + len_)
      grow(2);

   *(wptr_++) = ch;
   *wptr_ = '\0';
}
