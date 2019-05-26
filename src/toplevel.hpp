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
#include "ident.h"
#include "util/array.hpp"
#include "util/printer.hpp"

class Scope;
class Net;
class Signal;

typedef ArrayList<Scope *> ScopeList;
extern template class ArrayList<Scope *>;

typedef ArrayList<Signal *> SignalList;
extern template class ArrayList<Signal *>;

typedef ArrayList<Net *> NetList;
extern template class ArrayList<Net *>;

class TopLevel {
   friend class Elaborator;
public:

   Scope *root() const { return root_; }

   void print(Printer&& printer=StdoutPrinter()) const;

private:
   Scope   *root_ = nullptr;
   NetList  nets_;
};

class Scope {
   friend class Elaborator;
public:
   Scope(Scope *parent, ident_t name);

   Scope *parent() const { return parent_; }
   ident_t name() const { return name_; }
   const ScopeList& children() const { return children_; }
   const SignalList& signals() const { return signals_; }

   void print(Printer& printer, int indent=0) const;

private:
   void link_to(Scope *child);

   Scope      *parent_;
   ident_t     name_;
   ScopeList   children_;
   SignalList  signals_;
};

class Signal {
   friend class Elaborator;
public:
   explicit Signal(ident_t name);

   const NetList& nets() const { return nets_; }
   ident_t name() const { return name_; }

   void print(Printer& printer, int indent=0) const;

private:
   ident_t name_;
   NetList nets_;
};

class Net {
   friend class Elaborator;
public:
   Net(netid_t nid, unsigned nnets, unsigned size);

   netid_t nid() const { return nid_; }

private:
   SignalList signals_;
   netid_t    nid_;
   unsigned   nnets_;
   unsigned   size_;
};
