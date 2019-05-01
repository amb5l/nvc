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

class Bitmask {
public:
   explicit Bitmask(size_t size);
   ~Bitmask();

   void set(unsigned n);
   void clear(unsigned n);
   void zero();
   void one();
   bool is_set(unsigned n) const;
   int first_not_set() const;
   int first_set() const;

   size_t size() const { return size_; }

private:
   static inline size_t qwords(size_t bits);

   uint64_t *qwords_;
   size_t    size_;
};
