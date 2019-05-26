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

#include "stacktrace.hpp"

#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <cxxabi.h>

#ifdef HAVE_EXECINFO_H
#include <execinfo.h>
#endif

#ifdef HAVE_LIBDW
#include <elfutils/libdw.h>
#include <elfutils/libdwfl.h>
#include <dwarf.h>
#include <unwind.h>
#endif

template class ArrayList<Frame>;

Frame::Frame(Kind kind, const char *symbol, const char *file, int line,
             uintptr_t address, const char *module)
   : symbol_(xstrdup(symbol)),
     kind_(kind),
     file_(xstrdup(file)),
     line_(line),
     address_(address),
     module_(xstrdup(module))
{
}

Frame::Frame(const Frame& other)
   : symbol_(xstrdup(other.symbol_)),
     kind_(other.kind_),
     file_(xstrdup(other.file_)),
     line_(other.line_),
     address_(other.address_),
     module_(xstrdup(other.module_))
{
}

Frame::Frame(Frame&& other)
   : symbol_(other.symbol_),
     kind_(other.kind_),
     file_(other.file_),
     line_(other.line_),
     address_(other.address_),
     module_(other.module_)
{
   other.symbol_ = nullptr;
   other.file_ = nullptr;
   other.module_ = nullptr;
}

Frame::~Frame()
{
   free(symbol_);
}

#if defined HAVE_LIBDW

struct LibdwIterParams {
   int skip;
   FrameList *frames;
};

static bool die_has_pc(Dwarf_Die* die, Dwarf_Addr pc)
{
   Dwarf_Addr low, high;

   if (dwarf_hasattr(die, DW_AT_low_pc) && dwarf_hasattr(die, DW_AT_high_pc)) {
      if (dwarf_lowpc(die, &low) != 0)
         return false;
      if (dwarf_highpc(die, &high) != 0) {
         Dwarf_Attribute attr_mem;
         Dwarf_Attribute* attr = dwarf_attr(die, DW_AT_high_pc, &attr_mem);
         Dwarf_Word value;
         if (dwarf_formudata(attr, &value) != 0)
            return false;
         high = low + value;
      }
      return pc >= low && pc < high;
   }

   Dwarf_Addr base;
   ptrdiff_t offset = 0;
   while ((offset = dwarf_ranges(die, offset, &base, &low, &high)) > 0) {
      if (pc >= low && pc < high)
         return true;
   }

   return false;
}

static _Unwind_Reason_Code libdw_trace_iter(struct _Unwind_Context* ctx,
                                            void *param)
{
   static Dwfl *handle = NULL;
   static Dwfl_Module *home = NULL;

   LibdwIterParams *params = (LibdwIterParams *)param;

   if (handle == NULL) {
      static Dwfl_Callbacks callbacks = {
         .find_elf = dwfl_linux_proc_find_elf,
         .find_debuginfo = dwfl_standard_find_debuginfo,
         .debuginfo_path = NULL
      };

      if ((handle = dwfl_begin(&callbacks)) == NULL) {
         warnf("failed to initialise dwfl");
         return _URC_NORMAL_STOP;
      }

      dwfl_report_begin(handle);
      if (dwfl_linux_proc_report(handle, getpid()) < 0) {
         warnf("dwfl_linux_proc_report failed");
         return _URC_NORMAL_STOP;
      }
      dwfl_report_end(handle, NULL, NULL);

      home = dwfl_addrmodule(handle, (uintptr_t)libdw_trace_iter);
   }

   if (params->skip > 0) {
      (params->skip)--;
      return _URC_NO_REASON;
   }

   int ip_before_instruction = 0;
   uintptr_t ip = _Unwind_GetIPInfo(ctx, &ip_before_instruction);

   if (ip == 0)
      return _URC_NO_REASON;
   else if (!ip_before_instruction)
      ip -= 1;

   Dwfl_Module *mod = dwfl_addrmodule(handle, ip);

   const char *module_name = dwfl_module_info(mod, 0, 0, 0, 0, 0, 0, 0);
   const char *sym_name = dwfl_module_addrname(mod, ip);

   Dwarf_Addr mod_bias = 0;
   Dwarf_Die *die = dwfl_module_addrdie(mod, ip, &mod_bias);

   if (die == NULL) {
      // Hack to support Clang taken from backward-cpp
      while ((die = dwfl_module_nextcu(mod, die, &mod_bias))) {
         Dwarf_Die child;
         if (dwarf_child(die, &child) != 0)
            continue;

         Dwarf_Die* iter = &child;
         do {
            switch (dwarf_tag(iter)) {
            case DW_TAG_subprogram:
            case DW_TAG_inlined_subroutine:
               if (die_has_pc(iter, ip))
                  goto found_die_with_ip;
            }
         } while (dwarf_siblingof(iter, iter) == 0);
      }
   found_die_with_ip:
      ;
   }

   Dwarf_Line* srcloc = dwarf_getsrc_die(die, ip - mod_bias);
   const char* srcfile = dwarf_linesrc(srcloc, 0, 0);

   int line = 0, col = 0;
   dwarf_lineno(srcloc, &line);
   dwarf_linecol(srcloc, &col);

   Frame::Kind kind = Frame::C;
   int status;
   char *demangled LOCAL = abi::__cxa_demangle(sym_name, NULL, NULL, &status);
   if (status == 0) {
      sym_name = demangled;
      kind = Frame::CXX;
   }

   color_printf("[$green$%p$$] ", (void *)ip);
   if (mod != home)
      color_printf("($red$%s$$) ", module_name);
   if (srcfile != NULL)
      color_printf("%s:%d ", srcfile, line);
   if (sym_name != NULL)
      color_printf("$yellow$%s$$", sym_name);
   printf("\n");

   params->frames->add(Frame(kind, sym_name, srcfile, line, ip, module_name));

#if 0
   FILE *f = fopen(srcfile, "r");
   if (f != NULL) {
      char buf[TRACE_MAX_LINE];
      for (int i = 0; i < line + 1 && fgets(buf, sizeof(buf), f); i++) {
         if (i < line - 2)
            continue;

         const size_t len = strlen(buf);
         if (len <= 1)
            continue;
         else if (buf[len - 1] == '\n')
            buf[len - 1] = '\0';

         if (i == line - 1)
            color_printf("$cyan$$bold$-->$$ $cyan$%s$$\n", buf);
         else
            color_printf("    $cyan$%s$$\n", buf);
      }
      fclose(f);
   }
#endif

   if (sym_name != NULL && strcmp(sym_name, "main") == 0)
      return _URC_NORMAL_STOP;
   else
      return _URC_NO_REASON;
}
#endif  // HAVE_LIBDW

FrameList stack_trace()
{
   FrameList result;

#if defined HAVE_LIBDW
   LibdwIterParams params = { 1, &result };
   _Unwind_Backtrace(libdw_trace_iter, &params);
#elif defined HAVE_EXECINFO_H
   void *trace[N_TRACE_DEPTH];
   char **messages = NULL;
   int trace_size = 0;

   trace_size = backtrace(trace, N_TRACE_DEPTH);
   messages = backtrace_symbols(trace, trace_size);

   print_trace(messages, trace_size);

   free(messages);
#elif defined __WIN64
   CONTEXT context;
   RtlCaptureContext(&context);

   win64_stacktrace(&context);
#endif

   return result;
}
