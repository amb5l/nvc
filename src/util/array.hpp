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

#include <cassert>
#include <cstdlib>
#include <new>

template <typename T>
class ArrayList {
public:
   ArrayList();
   ArrayList(const ArrayList&) = delete;
   ArrayList(ArrayList&&);
   ~ArrayList();

   ArrayList<T>& add(const T& item);
   ArrayList<T>& add(T&& item);
   unsigned size() const { return count_; }

   T& get(unsigned n);
   const T& get(unsigned n) const;

   T& operator[](unsigned n) { return get(n); }
   const T& operator[](unsigned n) const { return get(n); }

   class Iterator {
      friend class ArrayList<T>;
   public:
      bool operator!=(const Iterator& other) const;
      Iterator& operator++();
      T& operator*() { return owner_->get(index_); }

   private:
      Iterator(ArrayList<T> *owner, int index);

      ArrayList<T> *owner_;
      unsigned      index_;
   };

   class ConstIterator {
      friend class ArrayList<T>;
   public:
      bool operator!=(const ConstIterator& other) const;
      ConstIterator& operator++();
      const T& operator*() const { return owner_->get(index_); }

   private:
      ConstIterator(const ArrayList<T> *owner, int index);

      const ArrayList<T> *owner_;
      unsigned            index_;
   };

   Iterator begin();
   Iterator end();

   ConstIterator begin() const;
   ConstIterator end() const;

private:
   static const unsigned DEFAULT_SIZE = 16;

   void ensure_capacity(unsigned how_many);

   T        *items_;
   unsigned  max_ = DEFAULT_SIZE;
   unsigned  count_ = 0;
};

template <typename T>
ArrayList<T>::ArrayList()
{
   items_ = (T*)xmalloc(max_ * sizeof(T));
}

template <typename T>
ArrayList<T>::ArrayList(ArrayList&& other)
{
   assert(false);
}

template <typename T>
ArrayList<T>::~ArrayList()
{
   for (unsigned i = 0; i < count_; i++)
      items_[i].~T();

   free(items_);
}

template <typename T>
void ArrayList<T>::ensure_capacity(unsigned how_many)
{
   if (unlikely(count_ + how_many > max_)) {
      max_ = (max_ * 3) / 2;

      T *tmp = (T*)xmalloc(max_ * sizeof(T));

      for (unsigned i = 0; i < count_; i++) {
         new (&(tmp[i])) T(std::move(items_[i]));
         items_[i].~T();
      }

      free(items_);
      items_ = tmp;
   }
}

template <typename T>
ArrayList<T>& ArrayList<T>::add(const T& item)
{
   ensure_capacity(1);

   new (&(items_[count_++])) T(item);
   return *this;
}

template <typename T>
ArrayList<T>& ArrayList<T>::add(T&& item)
{
   ensure_capacity(1);

   new (&(items_[count_++])) T(std::move(item));
   return *this;
}

template <typename T>
T& ArrayList<T>::get(unsigned n)
{
   assert(n < count_);
   return items_[n];
}

template <typename T>
const T& ArrayList<T>::get(unsigned n) const
{
   assert(n < count_);
   return items_[n];
}

template <typename T>
typename ArrayList<T>::Iterator ArrayList<T>::begin()
{
   return Iterator(this, 0);
}

template <typename T>
typename ArrayList<T>::Iterator ArrayList<T>::end()
{
   return Iterator(this, count_);
}

template <typename T>
typename ArrayList<T>::ConstIterator ArrayList<T>::begin() const
{
   return ConstIterator(this, 0);
}

template <typename T>
typename ArrayList<T>::ConstIterator ArrayList<T>::end() const
{
   return ConstIterator(this, count_);
}

template <typename T>
bool ArrayList<T>::Iterator::operator!=(const Iterator& other) const
{
   return owner_ != other.owner_ || index_ != other.index_;
}

template <typename T>
typename ArrayList<T>::Iterator& ArrayList<T>::Iterator::operator++()
{
   index_++;
   return *this;
}

template <typename T>
ArrayList<T>::Iterator::Iterator(ArrayList<T> *owner, int index)
   : owner_(owner),
     index_(index)
{
}

template <typename T>
bool ArrayList<T>::ConstIterator::operator!=(const ConstIterator& other) const
{
   return owner_ != other.owner_ || index_ != other.index_;
}

template <typename T>
typename ArrayList<T>::ConstIterator& ArrayList<T>::ConstIterator::operator++()
{
   index_++;
   return *this;
}

template <typename T>
ArrayList<T>::ConstIterator::ConstIterator(const ArrayList<T> *owner, int index)
   : owner_(owner),
     index_(index)
{
}
