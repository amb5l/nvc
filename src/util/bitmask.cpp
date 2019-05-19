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

#include "bitmask.hpp"

#include <assert.h>

inline size_t Bitmask::qwords(size_t bits)
{
   return (bits + 63) / 64;
}

Bitmask::Bitmask(size_t size)
   : qwords_(new uint64_t[qwords(size)]{}),
     size_(size)
{
   assert(size > 0);
}

Bitmask::~Bitmask()
{
   delete[] qwords_;
}

void Bitmask::set(unsigned n)
{
   assert(n < size_);
   qwords_[n >> 6] |= UINT64_C(1) << (n & 0x3f);
}

void Bitmask::clear(unsigned n)
{
   assert(n < size_);
   qwords_[n >> 6] &= ~(UINT64_C(1) << (n & 0x3f));
}

bool Bitmask::is_set(unsigned n) const
{
   assert(n < size_);
   return !!(qwords_[n >> 6] & (UINT64_C(1) << (n & 0x3f)));
}

bool Bitmask::is_clear(unsigned n) const
{
   assert(n < size_);
   return !(qwords_[n >> 6] & (UINT64_C(1) << (n & 0x3f)));
}

int Bitmask::first_clear() const
{
   for (size_t i = 0; i < qwords(size_); i++) {
      const int ffs = __builtin_ffsll(~qwords_[i]);
      if (ffs != 0) {
         const int result = i * 64 + ffs - 1;
         return result < (int)size_ ? result : -1;
      }
   }

   return -1;
}

int Bitmask::first_set() const
{
   for (size_t i = 0; i < qwords(size_); i++) {
      const int ffs = __builtin_ffsll(qwords_[i]);
      if (ffs != 0) {
         const int result = i * 64 + ffs - 1;
         return result < (int)size_ ? result : -1;
      }
   }

   return -1;
}

void Bitmask::zero()
{
   for (size_t i = 0; i < qwords(size_); i++)
      qwords_[i] = 0;
}

void Bitmask::one()
{
   const size_t nqwords = qwords(size_);
   for (size_t i = 0; i < nqwords; i++)
      qwords_[i] = ~UINT64_C(0);
}

bool Bitmask::all_clear() const
{
   return first_set() == -1;
}

bool Bitmask::all_set() const
{
   return first_clear() == -1;
}
