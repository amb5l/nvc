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

#include "array.hpp"

class Frame {
public:
   enum Kind { C, CXX };

   Frame() = default;
   Frame(Kind kind, const char *symbol, const char *file, int line,
         uintptr_t address, const char *module);
   Frame(const Frame&);
   Frame(Frame&&);
   ~Frame();

   const char *symbol() const { return symbol_; }
   Kind kind() const { return kind_; }
   int line() const { return line_; }
   const char *file() const { return file_; }
   const char *module() const { return module_; }
   uintptr_t address() const { return address_; }

private:
   char      *symbol_ = nullptr;
   Kind       kind_ = C;
   char      *file_ = nullptr;
   int        line_ = -1;
   uintptr_t  address_ = 0;
   char      *module_ = nullptr;
};

typedef ArrayList<Frame> FrameList;
extern template class ArrayList<Frame>;

FrameList stack_trace();
