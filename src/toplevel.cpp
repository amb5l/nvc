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

#include "toplevel.hpp"

#include <cassert>

template class ArrayList<Scope *>;
template class ArrayList<Net *>;
template class ArrayList<Signal *>;

Scope::Scope(Scope *parent, ident_t name)
   : parent_(parent),
     name_(name)
{
   if (parent != nullptr)
      parent->link_to(this);
}

void Scope::print(Printer& printer, int indent) const
{
   printer.repeat(' ', indent);
   printer.color_print("$bold$$green$scope$$ %s\n", istr(name_));

   for (Signal *signal : signals_)
      signal->print(printer, indent + 2);

   for (Scope *child : children_)
      child->print(printer, indent + 2);
}

void Scope::link_to(Scope *child)
{
   children_.add(child);
}

void TopLevel::print(Printer&& printer) const
{
   if (root_ != nullptr)
      root_->print(printer);
}

Signal::Signal(ident_t name)
   : name_(name)
{

}

void Signal::print(Printer& printer, int indent) const
{
   printer.repeat(' ', indent);
   printer.color_print("$bold$$blue$signal$$ %s [", istr(name_));

   bool first = true;
   for (Net *net : nets_) {
      if (!first)
         printer.print(", ");
      printer.print("%d", net->nid());
      first = false;
   }

   printer.print("]\n");
}

Net::Net(netid_t nid, unsigned nnets, unsigned size)
   : nid_(nid),
     nnets_(nnets),
     size_(size)
{

}
