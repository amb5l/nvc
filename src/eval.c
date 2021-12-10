//
//  Copyright (C) 2013-2021  Nick Gasson
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

#include "phase.h"
#include "util.h"
#include "common.h"
#include "vcode.h"
#include "exec.h"

#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <inttypes.h>
#include <math.h>
#include <float.h>

static void eval_load_vcode(lib_t lib, tree_t unit, eval_flags_t flags)
{
   ident_t unit_name = tree_ident(unit);

   if (flags & EVAL_VERBOSE)
      notef("loading vcode for %s", istr(unit_name));

   if (!lib_load_vcode(lib, unit_name)) {
      if (flags & EVAL_WARN)
         warnf("cannot load vcode for %s", istr(unit_name));
   }
}

static vcode_unit_t eval_find_unit(ident_t func_name, eval_flags_t flags)
{
   vcode_unit_t vcode = vcode_find_unit(func_name);
   if (vcode == NULL) {
      ident_t strip_type_suffix = ident_until(func_name, "("[0]);
      ident_t unit_name = ident_runtil(strip_type_suffix, '.');
      ident_t lib_name = ident_until(strip_type_suffix, '.');

      lib_t lib;
      if (lib_name != unit_name && (lib = lib_find(lib_name, false)) != NULL) {
         tree_t unit = lib_get(lib, unit_name);
         if (unit != NULL) {
            eval_load_vcode(lib, unit, flags);

            if (tree_kind(unit) == T_PACKAGE) {
               ident_t body_name =
                  ident_prefix(unit_name, ident_new("body"), '-');
               tree_t body = lib_get(lib, body_name);
               if (body != NULL)
                  eval_load_vcode(lib, body, flags);
            }

            vcode = vcode_find_unit(func_name);
         }
      }
   }

   if (vcode == NULL && (flags & EVAL_VERBOSE))
      warnf("could not find vcode for unit %s", istr(func_name));

   return vcode;
}

static bool eval_have_lowered(tree_t func, eval_flags_t flags)
{
   if (is_builtin(tree_subkind(func)))
      return true;
   else if (!tree_has_ident2(func))
      return false;

   ident_t mangled = tree_ident2(func);
   if (eval_find_unit(mangled, flags) == NULL) {
      if (!(flags & EVAL_LOWER))
         return false;
      else if (tree_kind(func) != T_FUNC_BODY)
         return false;

      return lower_func(func) != NULL;
   }
   else
      return true;
}

static bool eval_not_possible(tree_t t, eval_flags_t flags, const char *why)
{
   if (flags & EVAL_WARN)
      warn_at(tree_loc(t), "%s prevents constant folding", why);

   return false;
}

static bool eval_possible(tree_t t, eval_flags_t flags)
{
   switch (tree_kind(t)) {
   case T_FCALL:
      {
         tree_t decl = tree_ref(t);
         const subprogram_kind_t kind = tree_subkind(decl);
         if (kind == S_USER && !(flags & EVAL_FCALL))
            return eval_not_possible(t, flags, "call to user defined function");
         else if (kind == S_FOREIGN)
            return eval_not_possible(t, flags, "call to foreign function");
         else if (tree_flags(decl) & TREE_F_IMPURE)
            return eval_not_possible(t, flags, "call to impure function");
         else if (!(tree_flags(t) & TREE_F_GLOBALLY_STATIC))
            return eval_not_possible(t, flags, "non-static expression");

         const int nparams = tree_params(t);
         for (int i = 0; i < nparams; i++) {
            tree_t p = tree_value(tree_param(t, i));
            if (!eval_possible(p, flags))
               return false;
            else if ((flags & EVAL_FOLDING)
                     && tree_kind(p) == T_FCALL
                     && type_is_scalar(tree_type(p)))
               return false;  // Would have been folded already if possible
         }

         // This can actually lower the function on demand so only call
         // it if we know all the parameters can be evaluated now
         return eval_have_lowered(tree_ref(t), flags);
      }

   case T_LITERAL:
      return true;

   case T_TYPE_CONV:
      return eval_possible(tree_value(t), flags);

   case T_QUALIFIED:
      return eval_possible(tree_value(t), flags);

   case T_REF:
      {
         tree_t decl = tree_ref(t);
         switch (tree_kind(decl)) {
         case T_UNIT_DECL:
         case T_ENUM_LIT:
            return true;

         case T_CONST_DECL:
            if (tree_has_value(decl))
               return eval_possible(tree_value(decl), flags);
            else if (!(flags & EVAL_FCALL))
               return eval_not_possible(t, flags, "deferred constant");
            else
               return true;

         default:
            return eval_not_possible(t, flags, "reference");
         }
      }

   case T_RECORD_REF:
      return eval_possible(tree_value(t), flags);

   case T_AGGREGATE:
      {
         const int nassocs = tree_assocs(t);
         for (int i = 0; i < nassocs; i++) {
            if (!eval_possible(tree_value(tree_assoc(t, i)), flags))
               return false;
         }

         return true;
      }

   default:
      return eval_not_possible(t, flags, "aggregate");
   }
}

static bool eval_can_represent_type(type_t type)
{
   if (type_is_scalar(type))
      return true;
   else if (type_is_array(type))
      return eval_can_represent_type(type_elem(type));
   else if (type_is_record(type)) {
      const int nfields = type_fields(type);
      for (int i = 0; i < nfields; i++) {
         if (!eval_can_represent_type(tree_type(type_field(type, i))))
            return false;
      }
      return true;
   }
   else
      return false;
}

tree_t eval(tree_t expr, exec_t *ex)
{
   type_t type = tree_type(expr);
   if (!type_is_scalar(type))
      return expr;
   else if (!eval_can_represent_type(type))
      return expr;
   else if (!eval_possible(expr, exec_get_flags(ex)))
      return expr;

   vcode_unit_t thunk = lower_thunk(expr);
   if (thunk == NULL)
      return expr;

   tree_t tree = exec_fold(ex, expr, thunk);

   vcode_unit_unref(thunk);
   thunk = NULL;

   return tree;
}
