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

template <typename Key, typename Value>
class HashMap {
public:
   explicit HashMap(unsigned hint_size=DEFAULT_SIZE);
   HashMap(const HashMap&) = delete;
   HashMap(HashMap&&) = delete;
   ~HashMap();

   Value get(Key key) const;
   void put(Key key, Value value);

private:
   static const int DEFAULT_SIZE = 32;

   int hash_slot(Key key) const;
   void rehash();

   Key      *keys_;
   Value    *values_;
   unsigned  size_;
   unsigned  members_ = 0;
};

template <typename Key, typename Value>
HashMap<Key, Value>::HashMap(unsigned hint_size)
{
   size_ = next_power_of_2(hint_size * 2);
   keys_ = new Key[size_];
   values_ = new Value[size_];
}

template <typename Key, typename Value>
HashMap<Key, Value>::~HashMap()
{
   delete[] keys_;
   delete[] values_;
}

template <typename Key, typename Value>
Value HashMap<Key, Value>::get(Key key) const
{
   int slot = hash_slot(key);

   for (; ; slot = (slot + 1) & (size_ - 1)) {
      if (keys_[slot] == key)
         return values_[slot];
      else if (keys_[slot] == Key{})
         return Value{};
   }
}

template <typename Key, typename Value>
void HashMap<Key, Value>::rehash()
{
   // Rebuild the hash table with a larger size
   // This is expensive so a conservative initial size should be chosen

   const int old_size = size_;
   size_ *= 2;

   Key *old_keys = keys_;
   Value *old_values = values_;

   keys_ = new Key[size_];
   values_ = new Value[size_];

   members_ = 0;

   for (int i = 0; i < old_size; i++) {
      if (old_keys[i] != Key{})
         put(old_keys[i], old_values[i]);
   }

   delete[] old_keys;
   delete[] old_values;
}

template <typename Key, typename Value>
void HashMap<Key, Value>::put(Key key, Value value)
{
   if (unlikely(members_ >= size_ / 2))
      rehash();

   int slot = hash_slot(key);

   for (; ; slot = (slot + 1) & (size_ - 1)) {
      if (keys_[slot] == key)
         values_[slot] = value;
      else if (keys_[slot] == Key{}) {
         values_[slot] = value;
         keys_[slot] = key;
         members_++;
         break;
      }
   }
}

template <typename Key, typename Value>
int HashMap<Key, Value>::hash_slot(Key key) const
{
   uintptr_t uptr = (uintptr_t)key;

   // Bottom two bits will always be zero with 32-bit pointers
   uptr >>= 2;

   // Hash function from here:
   //   http://burtleburtle.net/bob/hash/integer.html

   uint32_t a = (uint32_t)uptr;
   a = (a ^ 61) ^ (a >> 16);
   a = a + (a << 3);
   a = a ^ (a >> 4);
   a = a * UINT32_C(0x27d4eb2d);
   a = a ^ (a >> 15);

   return a & (size_ - 1);
}
