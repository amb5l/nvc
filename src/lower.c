//
//  Copyright (C) 2014-2021  Nick Gasson
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

#include "util.h"
#include "phase.h"
#include "vcode.h"
#include "common.h"
#include "rt/rt.h"
#include "rt/cover.h"
#include "hash.h"
#include "array.h"

#include <assert.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <ctype.h>

typedef enum {
   EXPR_LVALUE,
   EXPR_RVALUE,
   EXPR_INPUT_ASPECT,
} expr_ctx_t;

typedef struct loop_stack  loop_stack_t;
typedef struct lower_scope lower_scope_t;

struct loop_stack {
   loop_stack_t  *up;
   ident_t        name;
   vcode_block_t  test_bb;
   vcode_block_t  exit_bb;
};

typedef enum {
   LOWER_NORMAL,
   LOWER_THUNK
} lower_mode_t;

typedef enum {
   SHORT_CIRCUIT_AND,
   SHORT_CIRCUIT_OR,
   SHORT_CIRCUIT_NOR
} short_circuit_op_t;

typedef enum {
   SCOPE_GLOBAL        = (1 << 0),
   SCOPE_HAS_PROTECTED = (1 << 1),
} scope_flags_t;

struct lower_scope {
   hash_t        *objects;
   lower_scope_t *down;
   scope_flags_t  flags;
   tree_t         hier;
   tree_t         container;
};

typedef enum {
   PART_ALL,
   PART_ELEM,
   PART_FIELD,
   PART_PUSH_FIELD,
   PART_PUSH_ELEM,
   PART_POP
} part_kind_t;

typedef struct {
   part_kind_t kind;
   vcode_reg_t reg;
   type_t      type;
} target_part_t;

static const char      *verbose = NULL;
static lower_mode_t     mode = LOWER_NORMAL;
static lower_scope_t   *top_scope = NULL;
static cover_tagging_t *cover_tags = NULL;

static vcode_reg_t lower_expr(tree_t expr, expr_ctx_t ctx);
static vcode_reg_t lower_reify_expr(tree_t expr);
static vcode_type_t lower_bounds(type_t type);
static void lower_stmt(tree_t stmt, loop_stack_t *loops);
static vcode_unit_t lower_func_body(tree_t body, vcode_unit_t context);
static void lower_proc_body(tree_t body, vcode_unit_t context);
static vcode_reg_t lower_signal_ref(tree_t decl, expr_ctx_t ctx);
static vcode_reg_t lower_record_aggregate(tree_t expr, bool nest,
                                          bool is_const, expr_ctx_t ctx);
static vcode_reg_t lower_param_ref(tree_t decl, expr_ctx_t ctx);
static vcode_type_t lower_type(type_t type);
static void lower_decls(tree_t scope, vcode_unit_t context);
static vcode_reg_t lower_array_dir(type_t type, int dim, vcode_reg_t reg);
static vcode_reg_t lower_concat(tree_t expr, expr_ctx_t ctx);
static vcode_reg_t lower_array_off(vcode_reg_t off, vcode_reg_t array,
                                   type_t type, unsigned dim);
static void lower_check_array_sizes(tree_t t, type_t ltype, type_t rtype,
                                    vcode_reg_t lval, vcode_reg_t rval);
static vcode_type_t lower_alias_type(tree_t alias);
static bool lower_const_bounds(type_t type);
static int lower_search_vcode_obj(void *key, lower_scope_t *scope, int *hops);
static type_t lower_elem_recur(type_t type);
static void lower_finished(void);
static void lower_predef(tree_t decl, vcode_unit_t context);
static ident_t lower_predef_func_name(type_t type, const char *op);

typedef vcode_reg_t (*lower_signal_flag_fn_t)(vcode_reg_t, vcode_reg_t);
typedef vcode_reg_t (*arith_fn_t)(vcode_reg_t, vcode_reg_t);

#define PUSH_DEBUG_INFO(t)                              \
   __attribute__((cleanup(emit_debug_info), unused))    \
   const loc_t _old_loc = *vcode_last_loc();            \
   emit_debug_info(tree_loc((t)));                      \

static bool lower_is_const(tree_t t)
{
   switch (tree_kind(t)) {
   case T_AGGREGATE:
      {
         bool is_const = true;
         type_t type = tree_type(t);
         if (type_is_array(type))
            is_const = lower_const_bounds(tree_type(t));
         const int nassocs = tree_assocs(t);
         for (int i = 0; i < nassocs; i++)
            is_const = is_const && lower_is_const(tree_value(tree_assoc(t, i)));
         return is_const;
      }

   case T_REF:
      {
         tree_t decl = tree_ref(t);
         const tree_kind_t decl_kind = tree_kind(decl);
         if (decl_kind == T_CONST_DECL && type_is_scalar(tree_type(t)))
            return !tree_has_value(decl) || lower_is_const(tree_value(decl));
         else
            return decl_kind == T_ENUM_LIT;
      }

   case T_LITERAL:
      return true;

   case T_RANGE:
      if (tree_subkind(t) == RANGE_EXPR)
         return lower_is_const(tree_value(t));
      else
         return lower_is_const(tree_left(t)) && lower_is_const(tree_right(t));

   default:
      return false;
   }
}

static bool lower_const_bounds(type_t type)
{
   assert(type_is_array(type));

   if (type_is_unconstrained(type))
      return false;
   else {
      const int ndims = dimension_of(type);
      for (int i = 0; i < ndims; i++) {
         tree_t r = range_of(type, i);
         switch (tree_subkind(r)) {
         case RANGE_TO:
         case RANGE_DOWNTO:
            if (!lower_is_const(tree_left(r)))
               return false;
            else if (!lower_is_const(tree_right(r)))
               return false;
            break;
         default:
            return false;
         }
      }

      type_t elem = type_elem(type);
      return type_is_array(elem) ? lower_const_bounds(elem) : true;
   }
}

static vcode_reg_t lower_range_expr(tree_t r)
{
   // The simplify pass should remove all RANGE_EXPR except A'RANGE
   // where A is an array with non-static bounds

   tree_t array = tree_name(tree_value(r));
   type_t type = tree_type(array);
   assert(!lower_const_bounds(type));
   return lower_expr(array, EXPR_RVALUE);
}

static bool lower_is_reverse_range(tree_t r)
{
   tree_t value = tree_value(r);
   assert(tree_kind(value) == T_ATTR_REF);
   return tree_subkind(value) == ATTR_REVERSE_RANGE;
}

static vcode_reg_t lower_range_left(tree_t r)
{
   assert(tree_kind(r) == T_RANGE);

   if (tree_subkind(r) == RANGE_EXPR) {
      vcode_reg_t array_reg = lower_range_expr(r), left_reg;
      if (lower_is_reverse_range(r))
         left_reg = emit_uarray_right(array_reg, 0);
      else
         left_reg = emit_uarray_left(array_reg, 0);

      vcode_type_t vtype = lower_type(tree_type(r));
      return emit_cast(vtype, vtype, left_reg);
   }
   else
      return lower_reify_expr(tree_left(r));
}

static vcode_reg_t lower_range_right(tree_t r)
{
   assert(tree_kind(r) == T_RANGE);

   if (tree_subkind(r) == RANGE_EXPR) {
      vcode_reg_t array_reg = lower_range_expr(r), right_reg;
      if (lower_is_reverse_range(r))
         right_reg = emit_uarray_left(array_reg, 0);
      else
         right_reg = emit_uarray_right(array_reg, 0);

      vcode_type_t vtype = lower_type(tree_type(r));
      return emit_cast(vtype, vtype, right_reg);
   }
   else
      return lower_reify_expr(tree_right(r));
}

static vcode_reg_t lower_range_dir(tree_t r)
{
   const range_kind_t rkind = tree_subkind(r);

   switch (rkind) {
   case RANGE_TO:
   case RANGE_DOWNTO:
      return emit_const(vtype_bool(), rkind);

   case RANGE_EXPR:
      {
         vcode_reg_t reg = lower_range_expr(r);

         tree_t value = tree_value(r);
         assert(tree_kind(value) == T_ATTR_REF);

         if (tree_subkind(value) == ATTR_REVERSE_RANGE)
            return emit_not(emit_uarray_dir(reg, 0));
         else
            return emit_uarray_dir(reg, 0);
      }

   case RANGE_ERROR:
      break;
   }

   return VCODE_INVALID_REG;
}

static vcode_reg_t lower_array_data(vcode_reg_t reg)
{
   vcode_type_t type = vcode_reg_type(reg);
   switch (vtype_kind(type)) {
   case VCODE_TYPE_UARRAY:
      return emit_unwrap(reg);

   case VCODE_TYPE_POINTER:
   case VCODE_TYPE_SIGNAL:
      return reg;

   case VCODE_TYPE_CARRAY:
      // TODO: not needed?
      return emit_cast(vtype_pointer(vtype_elem(type)),
                       VCODE_INVALID_TYPE, reg);

   default:
      vcode_dump();
      fatal_trace("invalid type in lower_array_data r%d", reg);
   }
}

static vcode_reg_t lower_array_left(type_t type, int dim, vcode_reg_t reg)
{
   if (type_is_unconstrained(type)) {
      assert(reg != VCODE_INVALID_REG);
      type_t index_type = index_type_of(type, dim);
      return emit_cast(lower_type(index_type), lower_bounds(index_type),
                       emit_uarray_left(reg, dim));
   }
   else
      return lower_range_left(range_of(type, dim));
}

static vcode_reg_t lower_array_right(type_t type, int dim, vcode_reg_t reg)
{
   if (type_is_unconstrained(type)) {
      assert(reg != VCODE_INVALID_REG);
      type_t index_type = index_type_of(type, dim);
      return emit_cast(lower_type(index_type), lower_bounds(index_type),
                       emit_uarray_right(reg, dim));
   }
   else
      return lower_range_right(range_of(type, dim));
}

static vcode_reg_t lower_array_dir(type_t type, int dim, vcode_reg_t reg)
{
   if (type_is_unconstrained(type)) {
      assert(reg != VCODE_INVALID_REG);
      assert(vcode_reg_kind(reg) == VCODE_TYPE_UARRAY);
      return emit_uarray_dir(reg, dim);
   }
   else {
      assert(!type_is_unconstrained(type));
      return lower_range_dir(range_of(type, dim));
   }
}

static vcode_reg_t lower_array_len(type_t type, int dim, vcode_reg_t reg)
{
   assert(type_is_array(type));

   if (type_is_unconstrained(type)) {
      assert(reg != VCODE_INVALID_REG);
      return emit_uarray_len(reg, dim);
   }
   else {
      tree_t r = range_of(type, dim);

      int64_t low, high;
      if (folded_bounds(r, &low, &high))
         return emit_const(vtype_offset(), MAX(high - low + 1, 0));

      vcode_reg_t left_reg  = lower_range_left(r);
      vcode_reg_t right_reg = lower_range_right(r);

      vcode_reg_t diff = VCODE_INVALID_REG;
      switch (tree_subkind(r)) {
      case RANGE_EXPR:
         return emit_uarray_len(lower_range_expr(r), 0);

      case RANGE_TO:
         diff = emit_sub(right_reg, left_reg);
         break;

      case RANGE_DOWNTO:
         diff = emit_sub(left_reg, right_reg);
         break;
      }

      vcode_reg_t inc_reg = emit_const(vcode_reg_type(diff), 1);
      vcode_reg_t len_reg = emit_add(diff, inc_reg);
      vcode_type_t offset_type = vtype_offset();
      vcode_reg_t cast_reg =
         emit_cast(offset_type, VCODE_INVALID_TYPE, len_reg);
      vcode_reg_t zero_reg = emit_const(offset_type, 0);
      vcode_reg_t neg_reg = emit_cmp(VCODE_CMP_LT, cast_reg, zero_reg);

      return emit_select(neg_reg, zero_reg, cast_reg);
   }
}

static vcode_reg_t lower_array_total_len(type_t type, vcode_reg_t reg)
{
   const int ndims = dimension_of(type);

   vcode_reg_t total = VCODE_INVALID_REG;
   for (int i = 0; i < ndims; i++) {
      vcode_reg_t this = lower_array_len(type, i, reg);
      if (total == VCODE_INVALID_REG)
         total = this;
      else
         total = emit_mul(this, total);
   }

   type_t elem = type_elem(type);
   if (type_is_array(elem))
      return emit_mul(total, lower_array_total_len(elem, VCODE_INVALID_REG));
   else
      return total;
}

static vcode_reg_t lower_scalar_sub_elements(type_t type, vcode_reg_t reg)
{
   assert(type_is_array(type));

   vcode_reg_t count_reg = lower_array_total_len(type, reg);

   type_t elem = lower_elem_recur(type);
   if (type_is_record(elem))
      return emit_mul(count_reg, emit_const(vtype_offset(), type_width(elem)));
   else
      return count_reg;
}

static int lower_array_const_size(type_t type)
{
   const int ndims = dimension_of(type);

   int size = 1;
   for (int i = 0; i < ndims; i++) {
      tree_t r = range_of(type, i);
      int64_t low, high;
      range_bounds(r, &low, &high);
      size *= MAX(high - low + 1, 0);
   }

   type_t elem = type_elem(type);
   return type_is_array(elem) ? size * lower_array_const_size(elem) : size;
}

static type_t lower_elem_recur(type_t type)
{
   while (type_is_array(type))
      type = type_elem(type);
   return type;
}

static vcode_type_t lower_array_type(type_t type)
{
   type_t elem = lower_elem_recur(type);

   vcode_type_t elem_type   = lower_type(elem);
   vcode_type_t elem_bounds = lower_bounds(elem);

   if (lower_const_bounds(type))
      return vtype_carray(lower_array_const_size(type), elem_type, elem_bounds);
   else
      return vtype_uarray(dimension_of(type), elem_type, elem_bounds);
}

static vcode_type_t lower_type(type_t type)
{
   switch (type_kind(type)) {
   case T_SUBTYPE:
      if (type_is_array(type))
         return lower_array_type(type);
      else
         return lower_type(type_base(type));

   case T_ARRAY:
         return lower_array_type(type);

   case T_PHYSICAL:
   case T_INTEGER:
      {
         tree_t r = type_dim(type, 0);
         int64_t low, high;
         const bool folded = folded_bounds(r, &low, &high);
         if (folded)
            return vtype_int(low, high);
         else
            return vtype_int(INT64_MIN, INT64_MAX);
      }

   case T_ENUM:
      return vtype_int(0, type_enum_literals(type) - 1);

   case T_RECORD:
      {
         ident_t name = type_ident(type);
         vcode_type_t record = vtype_find_named_record(name);
         if (record == VCODE_INVALID_TYPE) {
            vtype_named_record(name, NULL, 0);  // Forward-declare the name

            const int nfields = type_fields(type);
            vcode_type_t fields[nfields];
            for (int i = 0; i < nfields; i++)
               fields[i] = lower_type(tree_type(type_field(type, i)));

            record = vtype_named_record(name, fields, nfields);
         }

         return record;
      }

   case T_PROTECTED:
      return vtype_context(type_ident(type));

   case T_FILE:
      return vtype_file(lower_type(type_file(type)));

   case T_ACCESS:
      {
         type_t access = type_access(type);
         if (type_is_array(access) && lower_const_bounds(access))
            return vtype_access(lower_type(lower_elem_recur(access)));
         else
            return vtype_access(lower_type(access));
      }

   case T_REAL:
      return vtype_real();

   case T_INCOMPLETE:
      return vtype_opaque();

   default:
      fatal("cannot lower type kind %s", type_kind_str(type_kind(type)));
   }
}

static vcode_type_t lower_bounds(type_t type)
{
   if (type_kind(type) == T_SUBTYPE
       && (type_is_integer(type) || type_is_enum(type))) {
      tree_t r = range_of(type, 0);
      int64_t low, high;
      if (folded_bounds(r, &low, &high))
         return vtype_int(low, high);
   }
   else if (type_is_array(type))
      return lower_bounds(type_elem(type));

   return lower_type(type);
}

static vcode_type_t lower_signal_type(type_t type)
{
   if (type_is_array(type)) {
      vcode_type_t base = vtype_signal(lower_type(lower_elem_recur(type)));
      if (lower_const_bounds(type))
         return base;
      else
         return vtype_uarray(dimension_of(type), base, base);
   }
   else
      return vtype_signal(lower_type(type));
}

static vcode_reg_t lower_reify(vcode_reg_t reg)
{
   if (reg == VCODE_INVALID_REG)
      return reg;

   switch (vtype_kind(vcode_reg_type(reg))) {
   case VCODE_TYPE_POINTER:
      return emit_load_indirect(reg);
   case VCODE_TYPE_SIGNAL:
     return emit_load_indirect(emit_resolved(reg));
   default:
      return reg;
   }
}

static vcode_reg_t lower_reify_expr(tree_t expr)
{
   return lower_reify(lower_expr(expr, EXPR_RVALUE));
}

static vcode_reg_t lower_wrap_with_new_bounds(type_t type, vcode_reg_t array,
                                              vcode_reg_t data)
{
   assert(type_is_array(type));

   const int ndims = dimension_of(type);
   vcode_dim_t dims[ndims];
   for (int i = 0; i < ndims; i++) {
      dims[i].left  = lower_array_left(type, i, array);
      dims[i].right = lower_array_right(type, i, array);
      dims[i].dir   = lower_array_dir(type, i, array);
   }

   return emit_wrap(lower_array_data(data), dims, ndims);
}

static vcode_reg_t lower_wrap(type_t type, vcode_reg_t data)
{
   return lower_wrap_with_new_bounds(type, data, data);
}

static bounds_kind_t lower_type_bounds_kind(type_t type)
{
   if (type_is_enum(type))
      return BOUNDS_ENUM;
   else
      return direction_of(type, 0) == RANGE_TO
         ? BOUNDS_TYPE_TO : BOUNDS_TYPE_DOWNTO;
}

static bool lower_scalar_has_static_bounds(type_t type, vcode_reg_t *low_reg,
                                           vcode_reg_t *high_reg)
{
   if (type_is_real(type))
      return true;  // TODO

   switch (type_kind(type)) {
   case T_INTEGER:
   case T_SUBTYPE:
      {
         tree_t r = range_of(type, 0);
         int64_t low, high;
         if (!folded_bounds(r, &low, &high)) {
            vcode_reg_t dir_reg   = lower_range_dir(r);
            vcode_reg_t left_reg  = lower_range_left(r);
            vcode_reg_t right_reg = lower_range_right(r);

            *low_reg  = emit_select(dir_reg, right_reg, left_reg);
            *high_reg = emit_select(dir_reg, left_reg, right_reg);
            return false;
         }
      }
      break;

   case T_ENUM:
   case T_PHYSICAL:
      break;

   default:
      fatal_trace("invalid type kind %s in lower_scalar_has_static_bounds",
                  type_kind_str(type_kind(type)));
   }

   *low_reg = *high_reg = VCODE_INVALID_TYPE;
   return true;
}

static char *lower_get_hint_string(tree_t where, const char *prefix)
{
   switch (tree_kind(where)) {
   case T_PORT_DECL:
      return xasprintf("%s|for parameter %s",
                       prefix ?: "", istr(tree_ident(where)));
   case T_VAR_DECL:
      return xasprintf("%s|for variable %s",
                       prefix ?: "", istr(tree_ident(where)));
   default:
      return prefix ? xstrdup(prefix) : NULL;
   }
}

static void lower_check_scalar_bounds(vcode_reg_t value, type_t type,
                                      tree_t where, tree_t hint)
{
   PUSH_DEBUG_INFO(tree_kind(where) == T_PORT_DECL ? hint : where);

   const bounds_kind_t kind = lower_type_bounds_kind(type);
   const char *prefix = kind == BOUNDS_ENUM ? type_pp(type) : NULL;
   char *hint_str LOCAL = lower_get_hint_string(where, prefix);

   vcode_reg_t low_reg, high_reg;
   if (lower_scalar_has_static_bounds(type, &low_reg, &high_reg))
      emit_bounds(value, lower_bounds(type), kind, hint_str);
   else {
      vcode_reg_t kind_reg = emit_const(vtype_offset(), kind);
      emit_dynamic_bounds(value, low_reg, high_reg, kind_reg, hint_str);
   }
}

static bool lower_have_signal(vcode_reg_t reg)
{
   const vtype_kind_t reg_kind = vcode_reg_kind(reg);
   return reg_kind == VCODE_TYPE_SIGNAL
      || (reg_kind == VCODE_TYPE_UARRAY
          && vtype_kind(vtype_elem(vcode_reg_type(reg))) == VCODE_TYPE_SIGNAL);
}

static vcode_reg_t lower_coerce_arrays(type_t from, type_t to, vcode_reg_t reg)
{
   const bool have_uarray = vcode_reg_kind(reg) == VCODE_TYPE_UARRAY;
   const bool need_uarray = !lower_const_bounds(to);

   if (have_uarray && need_uarray)
      return reg;
   else if (!have_uarray && need_uarray) {
      // Need to wrap array with metadata
      return lower_wrap(from, reg);
   }
   else if (have_uarray && !need_uarray) {
      // Need to unwrap array to get raw pointer
      return emit_unwrap(reg);
   }
   else
      return reg;
}

static vcode_reg_t lower_param(tree_t value, tree_t port, port_mode_t mode)
{
   type_t value_type = tree_type(value);

   class_t class = C_DEFAULT;
   type_t port_type = value_type;
   if (port != NULL) {
      port_type = tree_type(port);
      class = tree_class(port);
   }

   const bool must_reify =
      (type_is_scalar(value_type) || type_is_access(value_type)
       || type_is_file(value_type))
      && mode == PORT_IN;

   const bool lvalue = class == C_SIGNAL || class == C_FILE || mode != PORT_IN;

   vcode_reg_t reg = lower_expr(value, lvalue ? EXPR_LVALUE : EXPR_RVALUE);
   if (reg == VCODE_INVALID_REG)
      return reg;

   if (lower_have_signal(reg) && class != C_SIGNAL) {
      vcode_reg_t new_reg = emit_resolved(lower_array_data(reg));

      if (vcode_reg_kind(reg) == VCODE_TYPE_UARRAY)
         reg = lower_wrap_with_new_bounds(value_type, reg, new_reg);
      else
         reg = new_reg;
   }

   if (type_is_array(value_type)) {
      if (!type_is_unconstrained(port_type))
         lower_check_array_sizes(port, port_type, value_type,
                                 VCODE_INVALID_REG, reg);
      return lower_coerce_arrays(value_type, port_type, reg);
   }
   else if (class == C_SIGNAL || class == C_FILE)
      return reg;
   else {
      vcode_reg_t final = must_reify ? lower_reify(reg) : reg;
      if (mode != PORT_OUT && port != NULL && type_is_scalar(port_type))
         lower_check_scalar_bounds(lower_reify(final), port_type, port, value);
      return final;
   }
}

static vcode_reg_t lower_subprogram_arg(tree_t fcall, unsigned nth)
{
   if (nth >= tree_params(fcall))
      return VCODE_INVALID_REG;

   tree_t param = tree_param(fcall, nth);

   assert(tree_subkind(param) == P_POS);
   assert(tree_pos(param) == nth);

   tree_t value = tree_value(param);
   tree_t decl = tree_ref(fcall);

   port_mode_t mode = PORT_IN;
   class_t class = C_DEFAULT;
   if (nth < tree_ports(decl)) {
      tree_t port = tree_port(decl, nth);
      mode  = tree_subkind(port);
      class = tree_class(port);
   }

   tree_t port = NULL;
   if (!is_open_coded_builtin(tree_subkind(decl)))
      port = tree_port(decl, nth);

   vcode_reg_t preg = lower_param(value, port, mode);

   if ((mode == PORT_OUT || mode == PORT_INOUT) && class == C_SIGNAL
       && vcode_unit_kind() == VCODE_UNIT_PROCESS) {

      // LRM 08 section 4.2.2.3: a process statement contains a driver
      // for each actual signal associated with a formal signal
      // parameter of mode out or inout in a subprogram call.
      type_t type = tree_type(value);
      vcode_reg_t nets_reg = preg;
      if (type_is_array(type))
         nets_reg = lower_array_data(preg);

      vcode_reg_t count_reg = emit_const(vtype_offset(), type_width(type));
      emit_drive_signal(nets_reg, count_reg);
   }

   return preg;
}

static vcode_reg_t lower_signal_flag(tree_t ref, lower_signal_flag_fn_t fn)
{
   vcode_reg_t nets = lower_expr(ref, EXPR_INPUT_ASPECT);
   if (nets == VCODE_INVALID_REG)
      return emit_const(vtype_bool(), 0);

   vcode_reg_t length = VCODE_INVALID_REG;
   type_t type = tree_type(ref);
   if (type_is_array(type))
      length = lower_array_total_len(type, nets);
   else
      length = emit_const(vtype_offset(), 1);

   return (*fn)(nets, length);
}

static vcode_reg_t lower_last_value(tree_t ref)
{
   vcode_reg_t nets = lower_expr(ref, EXPR_LVALUE);

   type_t type = tree_type(ref);
   if (type_is_array(type) && !lower_const_bounds(type)) {
      assert(vcode_reg_kind(nets) == VCODE_TYPE_UARRAY);
      vcode_reg_t last_reg = emit_last_value(emit_unwrap(nets));
      return lower_wrap_with_new_bounds(type, nets, last_reg);
   }
   else
      return emit_last_value(nets);
}


static type_t lower_arg_type(tree_t fcall, int nth)
{
   if (nth >= tree_params(fcall))
      return NULL;
   else
      return tree_type(tree_value(tree_param(fcall, nth)));
}

static vcode_reg_t lower_min_max(vcode_cmp_t cmp, tree_t fcall)
{
   vcode_reg_t result = VCODE_INVALID_REG;

   const int nparams = tree_params(fcall);
   for (int i = 0; i < nparams; i++) {
      vcode_reg_t value = lower_subprogram_arg(fcall, i);
      if (result == VCODE_INVALID_REG)
         result = value;
      else {
         vcode_reg_t test = emit_cmp(cmp, value, result);
         result = emit_select(test, value, result);
      }
   }

   return result;
}

static vcode_reg_t lower_wrap_string(const char *str)
{
   const size_t len = strlen(str);
   vcode_reg_t chars[len + 1];
   vcode_type_t ctype = vtype_char();

   for (int j = 0; j < len; j++)
      chars[j] = emit_const(ctype, str[j]);

   vcode_type_t str_type = vtype_carray(len, ctype, ctype);
   vcode_reg_t data = emit_const_array(str_type, chars, len);

   vcode_dim_t dim0 = {
      .left  = emit_const(vtype_offset(), 1),
      .right = emit_const(vtype_offset(), len),
      .dir   = emit_const(vtype_bool(), RANGE_TO)
   };
   return emit_wrap(emit_address_of(data), &dim0, 1);
}

static vcode_reg_t lower_name_attr(tree_t ref, attr_kind_t which)
{
   tree_t decl = tree_ref(ref);

   if (which == ATTR_SIMPLE_NAME)
      return lower_wrap_string(istr(ident_downcase(tree_ident(decl))));
   else if (mode == LOWER_THUNK)
      return emit_undefined(vtype_uarray(1, vtype_char(), vtype_char()));

   switch (tree_kind(decl)) {
   case T_PACKAGE:
      {
         ident_t prefix = ident_prefix(tree_ident(decl), ident_new(":"), '\0');
         return lower_wrap_string(package_signal_path_name(prefix));
      }

   case T_PACK_BODY:
      {
         ident_t pack = ident_strip(tree_ident(decl), ident_new("-body"));
         ident_t prefix = ident_prefix(pack, ident_new(":"), '\0');
         return lower_wrap_string(package_signal_path_name(prefix));
      }

   case T_BLOCK:
      {
         tree_t d0 = tree_decl(decl, 0);
         assert(tree_kind(d0) == T_HIER);

         ident_t prefix;
         if (which == ATTR_PATH_NAME)
            prefix = tree_ident(d0);
         else
            prefix = tree_ident2(d0);

         ident_t full = ident_prefix(prefix, ident_new(":"), '\0');
         return lower_wrap_string(istr(full));
      }

   case T_PROCESS:
      {
         lower_scope_t *scope = top_scope;
         while (scope->hier == NULL)
            scope = scope->down;

         ident_t pname = tree_ident(decl);
         if (tree_flags(decl) & TREE_F_SYNTHETIC_NAME)
            pname = ident_new(":");
         else
            pname = ident_prefix(ident_downcase(pname), ident_new(":"), '\0');

         ident_t prefix;
         if (which == ATTR_PATH_NAME)
            prefix = tree_ident(scope->hier);
         else
            prefix = tree_ident2(scope->hier);

         return lower_wrap_string(istr(ident_prefix(prefix, pname, ':')));
      }

   case T_PROC_DECL:
   case T_FUNC_DECL:
   case T_PROC_BODY:
   case T_FUNC_BODY:
      {
         lower_scope_t *scope = top_scope;
         while (scope && scope->hier == NULL)
            scope = scope->down;

         if (scope == NULL) {
            const char *path = package_signal_path_name(tree_ident2(decl));
            return lower_wrap_string(path);
         }
         else {
            ident_t suffix = ident_prefix(ident_downcase(tree_ident(decl)),
                                          ident_new(":"), '\0');

            ident_t prefix;
            if (which == ATTR_PATH_NAME)
               prefix = tree_ident(scope->hier);
            else
               prefix = tree_ident2(scope->hier);

            return lower_wrap_string(istr(ident_prefix(prefix, suffix, ':')));
         }
      }

   case T_VAR_DECL:
   case T_SIGNAL_DECL:
   case T_ALIAS:
   case T_PORT_DECL:
   case T_CONST_DECL:
      {
         int hops, obj = lower_search_vcode_obj(decl, top_scope, &hops);
         if (obj == -1)
            return lower_wrap_string(package_signal_path_name(tree_ident2(decl)));

         vcode_state_t state;
         vcode_state_save(&state);

         lower_scope_t *scope = top_scope;
         while (hops--) {
            scope = scope->down;
            vcode_select_unit(vcode_unit_context());
         }

         obj &= 0x1fffffff;
         ident_t var_name = vcode_var_name((vcode_var_t)obj);

         vcode_state_restore(&state);

         if (tree_kind(decl) != T_PORT_DECL && var_name == tree_ident2(decl))
            return lower_wrap_string(package_signal_path_name(var_name));
         else {
            ident_t suffix = ident_downcase(tree_ident(decl));
            while (scope && scope->hier == NULL) {
               const bool synthetic =
                  tree_kind(scope->container) == T_PROCESS
                  && (tree_flags(scope->container) & TREE_F_SYNTHETIC_NAME);

               if (synthetic)
                  suffix = ident_prefix(ident_new(":"), suffix, '\0');
               else if (tree_kind(scope->container) == T_PACK_BODY) {
                  ident_t base = ident_strip(tree_ident(scope->container),
                                             ident_new("-body"));
                  suffix = ident_prefix(base, suffix, ':');
               }
               else {
                  ident_t simple = ident_downcase(tree_ident(scope->container));
                  suffix = ident_prefix(simple, suffix, ':');
               }
               scope = scope->down;
            }

            if (scope == NULL)
               return lower_wrap_string(package_signal_path_name(suffix));

            ident_t id = NULL;
            switch (which) {
            case ATTR_PATH_NAME:     id = tree_ident(scope->hier); break;
            case ATTR_INSTANCE_NAME: id = tree_ident2(scope->hier); break;
            default: assert(false);
            }

            id = ident_prefix(id, suffix, ':');
            return lower_wrap_string(istr(id));
         }
      }

   default:
      fatal_trace("cannot handle decl kind %s in lower_name_attr",
                  tree_kind_str(tree_kind(decl)));
   }
}

static vcode_reg_t lower_narrow(type_t result, vcode_reg_t reg)
{
   // Resize arithmetic result to width of target type

   vcode_type_t vtype = lower_type(result);
   if (!vtype_eq(vtype, vcode_reg_type(reg)))
      return emit_cast(vtype, lower_bounds(result), reg);
   else
      return reg;
}

static vcode_reg_t lower_arith(tree_t fcall, arith_fn_t fn, vcode_reg_t r0,
                               vcode_reg_t r1)
{
   vcode_type_t r0_type = vcode_reg_type(r0);
   vcode_type_t r1_type = vcode_reg_type(r1);
   if (!vtype_eq(r0_type, r1_type)) {
      const unsigned r0_bits = bits_for_range(vtype_low(r0_type),
                                              vtype_high(r0_type));
      const unsigned r1_bits = bits_for_range(vtype_low(r1_type),
                                              vtype_high(r1_type));

      if (r1_bits > r0_bits)
         r0 = emit_cast(r1_type, vcode_reg_bounds(r0), r0);
      else
         r1 = emit_cast(r0_type, vcode_reg_bounds(r1), r1);
   }

   return lower_narrow(tree_type(fcall), (*fn)(r0, r1));
}

static void lower_cond_coverage(tree_t test, vcode_reg_t value)
{
   int32_t cover_tag, sub_cond;
   if (cover_is_tagged(cover_tags, test, &cover_tag, &sub_cond))
      emit_cover_cond(value, cover_tag, sub_cond);
}

static vcode_reg_t lower_logical(tree_t fcall, vcode_reg_t result)
{
   int32_t cover_tag, sub_cond;
   if (!cover_is_tagged(cover_tags, fcall, &cover_tag, &sub_cond))
      return result;

   if (sub_cond > 0)
      emit_cover_cond(result, cover_tag, sub_cond);

   return result;
}

static bool lower_trivial_expression(tree_t expr)
{
   // True if expression is side-effect free with no function calls
   switch (tree_kind(expr)) {
   case T_REF:
   case T_LITERAL:
      return true;
   case T_FCALL:
      {
         if (!is_builtin(tree_subkind(tree_ref(expr))))
            return false;

         const int nparams = tree_params(expr);
         for (int i = 0; i < nparams; i++) {
            if (!lower_trivial_expression(tree_value(tree_param(expr, i))))
               return false;
         }

         return true;
      }
      break;
   default:
      return false;
   }
}

static vcode_reg_t lower_falling_rising_edge(tree_t fcall,
                                             subprogram_kind_t kind)
{
   tree_t p0 = tree_value(tree_param(fcall, 0));

   vcode_reg_t nets_reg  = lower_expr(p0, EXPR_LVALUE);
   vcode_reg_t value_reg = lower_reify(lower_expr(p0, EXPR_RVALUE));

   if (kind == S_FALLING_EDGE)
      value_reg = emit_not(value_reg);

   vcode_reg_t event_reg =
      emit_event_flag(nets_reg, emit_const(vtype_offset(), 1));
   vcode_reg_t r = emit_and(event_reg, value_reg);
   return r;
}

static vcode_reg_t lower_short_circuit(tree_t fcall, short_circuit_op_t op)
{
   vcode_reg_t r0 = lower_subprogram_arg(fcall, 0);

   int64_t value;
   if (vcode_reg_const(r0, &value)) {
      vcode_reg_t result = VCODE_INVALID_REG;
      switch (op) {
      case SHORT_CIRCUIT_AND:
         result = value ? lower_subprogram_arg(fcall, 1) : r0;
         break;
      case SHORT_CIRCUIT_OR:
         result = value ? r0 : lower_subprogram_arg(fcall, 1);
         break;
      case SHORT_CIRCUIT_NOR:
         result = emit_not(value ? r0 : lower_subprogram_arg(fcall, 1));
         break;
      }

      return lower_logical(fcall, result);
   }

   if (lower_trivial_expression(tree_value(tree_param(fcall, 1)))) {
      vcode_reg_t r1 = lower_subprogram_arg(fcall, 1);
      switch (op) {
      case SHORT_CIRCUIT_AND: return lower_logical(fcall, emit_and(r0, r1));
      case SHORT_CIRCUIT_OR: return lower_logical(fcall, emit_or(r0, r1));
      case SHORT_CIRCUIT_NOR: return lower_logical(fcall, emit_nor(r0, r1));
      }
   }

   vcode_block_t arg1_bb = emit_block();
   vcode_block_t after_bb = emit_block();

   vcode_type_t vbool = vtype_bool();
   vcode_reg_t result_reg = emit_alloca(vbool, vbool, VCODE_INVALID_REG);
   if (op == SHORT_CIRCUIT_NOR)
      emit_store_indirect(emit_not(r0), result_reg);
   else
      emit_store_indirect(r0, result_reg);

   if (op == SHORT_CIRCUIT_AND)
      emit_cond(r0, arg1_bb, after_bb);
   else
      emit_cond(r0, after_bb, arg1_bb);

   vcode_select_block(arg1_bb);
   vcode_reg_t r1 = lower_subprogram_arg(fcall, 1);

   switch (op) {
   case SHORT_CIRCUIT_AND:
      emit_store_indirect(emit_and(r0, r1), result_reg);
      break;
   case SHORT_CIRCUIT_OR:
      emit_store_indirect(emit_or(r0, r1), result_reg);
      break;
   case SHORT_CIRCUIT_NOR:
      emit_store_indirect(emit_nor(r0, r1), result_reg);
      break;
   }

   emit_jump(after_bb);

   vcode_select_block(after_bb);
   vcode_reg_t result = emit_load_indirect(result_reg);
   return lower_logical(fcall, result);
}

static vcode_reg_t lower_builtin(tree_t fcall, subprogram_kind_t builtin)
{
   if (builtin == S_INDEX_MAX)
      return lower_min_max(VCODE_CMP_GT, fcall);
   else if (builtin == S_INDEX_MIN)
      return lower_min_max(VCODE_CMP_LT, fcall);
   else if (builtin == S_SCALAR_AND)
      return lower_short_circuit(fcall, SHORT_CIRCUIT_AND);
   else if (builtin == S_SCALAR_OR)
      return lower_short_circuit(fcall, SHORT_CIRCUIT_OR);
   else if (builtin == S_SCALAR_NOR)
      return lower_short_circuit(fcall, SHORT_CIRCUIT_NOR);
   else if (builtin == S_CONCAT)
      return lower_concat(fcall, EXPR_RVALUE);
   else if (builtin == S_RISING_EDGE || builtin == S_FALLING_EDGE)
      return lower_falling_rising_edge(fcall, builtin);

   vcode_reg_t r0 = lower_subprogram_arg(fcall, 0);
   vcode_reg_t r1 = lower_subprogram_arg(fcall, 1);

   type_t r0_type = lower_arg_type(fcall, 0);
   type_t r1_type = lower_arg_type(fcall, 1);

   switch (builtin) {
   case S_SCALAR_EQ:
      return lower_logical(fcall, emit_cmp(VCODE_CMP_EQ, r0, r1));
   case S_SCALAR_NEQ:
      return lower_logical(fcall, emit_cmp(VCODE_CMP_NEQ, r0, r1));
   case S_SCALAR_LT:
      return lower_logical(fcall, emit_cmp(VCODE_CMP_LT, r0, r1));
   case S_SCALAR_GT:
      return lower_logical(fcall, emit_cmp(VCODE_CMP_GT, r0, r1));
   case S_SCALAR_LE:
      return lower_logical(fcall, emit_cmp(VCODE_CMP_LEQ, r0, r1));
   case S_SCALAR_GE:
      return lower_logical(fcall, emit_cmp(VCODE_CMP_GEQ, r0, r1));
   case S_MUL:
      return lower_arith(fcall, emit_mul, r0, r1);
   case S_ADD:
      return lower_arith(fcall, emit_add, r0, r1);
   case S_SUB:
      return lower_arith(fcall, emit_sub, r0, r1);
   case S_DIV:
      if (!type_eq(r0_type, r1_type))
         r1 = emit_cast(lower_type(r0_type), lower_bounds(r0_type), r1);
      return lower_narrow(tree_type(fcall), emit_div(r0, r1));
   case S_EXP:
      if (!type_eq(r0_type, r1_type))
         r1 = emit_cast(lower_type(r0_type), lower_bounds(r0_type), r1);
      return lower_arith(fcall, emit_exp, r0, r1);
   case S_MOD:
      return lower_arith(fcall, emit_mod, r0, r1);
   case S_REM:
      return lower_arith(fcall, emit_rem, r0, r1);
   case S_NEGATE:
      return emit_neg(r0);
   case S_ABS:
      return emit_abs(r0);
   case S_IDENTITY:
      return r0;
   case S_SCALAR_NOT:
      return lower_logical(fcall, emit_not(r0));
   case S_SCALAR_XOR:
      return lower_logical(fcall, emit_xor(r0, r1));
   case S_SCALAR_XNOR:
      return lower_logical(fcall, emit_xnor(r0, r1));
   case S_SCALAR_NAND:
      return lower_logical(fcall, emit_nand(r0, r1));
   case S_ENDFILE:
      return emit_endfile(r0);
   case S_FILE_OPEN1:
      {
         vcode_reg_t name   = lower_array_data(r1);
         vcode_reg_t length = lower_array_len(r1_type, 0, r1);
         emit_file_open(r0, name, length, lower_subprogram_arg(fcall, 2),
                        VCODE_INVALID_REG);
         return VCODE_INVALID_REG;
      }
   case S_FILE_OPEN2:
      {
         vcode_reg_t r2     = lower_subprogram_arg(fcall, 2);
         vcode_reg_t name   = lower_array_data(r2);
         vcode_reg_t length = lower_array_len(lower_arg_type(fcall, 2), 0, r2);
         emit_file_open(r1, name, length, lower_subprogram_arg(fcall, 3), r0);
         return VCODE_INVALID_REG;
      }
   case S_FILE_WRITE:
      {
         vcode_reg_t length = VCODE_INVALID_REG;
         vcode_reg_t data   = r1;
         if (type_is_array(r1_type)) {
            length = lower_array_len(r1_type, 0, r1);
            data   = lower_array_data(r1);
         }
         emit_file_write(r0, data, length);
         return VCODE_INVALID_REG;
      }
   case S_FILE_CLOSE:
      emit_file_close(r0);
      return VCODE_INVALID_REG;
   case S_FILE_READ:
      {
         vcode_reg_t inlen = VCODE_INVALID_REG;
         if (type_is_array(r1_type))
            inlen = lower_array_len(r1_type, 0, r1);

         vcode_reg_t outlen = VCODE_INVALID_REG;
         if (tree_params(fcall) == 3)
            outlen = lower_subprogram_arg(fcall, 2);

         emit_file_read(r0, r1, inlen, outlen);
         return VCODE_INVALID_REG;
      }
   case S_FILE_FLUSH:
      {
         ident_t func = ident_new("__nvc_flush");
         vcode_reg_t args[] = { r0 };
         emit_fcall(func, VCODE_INVALID_TYPE, VCODE_INVALID_TYPE,
                    VCODE_CC_FOREIGN, args, 1);
         return VCODE_INVALID_REG;
      }
   case S_DEALLOCATE:
      emit_deallocate(r0);
      return VCODE_INVALID_REG;
   case S_MUL_RP:
   case S_MUL_RI:
      {
         vcode_type_t vreal = vtype_real();
         vcode_type_t rtype = lower_type(tree_type(fcall));
         return emit_cast(rtype, rtype,
                          emit_mul(r0, emit_cast(vreal, vreal, r1)));
      }
   case S_MUL_PR:
   case S_MUL_IR:
      {
         vcode_type_t vreal = vtype_real();
         vcode_type_t rtype = lower_type(tree_type(fcall));
         return emit_cast(rtype, rtype,
                          emit_mul(emit_cast(vreal, vreal, r0), r1));
      }
   case S_DIV_PR:
      {
         vcode_type_t vreal = vtype_real();
         vcode_type_t rtype = lower_type(tree_type(fcall));
         return emit_cast(rtype, rtype,
                          emit_div(emit_cast(vreal, vreal, r0), r1));
      }
   case S_DIV_RI:
      {
         vcode_type_t vreal = vtype_real();
         vcode_type_t rtype = lower_type(tree_type(fcall));
         return emit_cast(rtype, rtype,
                          emit_div(r0, emit_cast(vreal, vreal, r1)));
      }
   default:
      fatal_at(tree_loc(fcall), "cannot lower builtin %d", builtin);
   }
}

static vcode_type_t lower_func_result_type(type_t result)
{
   if (type_is_array(result) && lower_const_bounds(result)) {
      return vtype_pointer(lower_type(lower_elem_recur(result)));
   }
   if (type_is_record(result))
      return vtype_pointer(lower_type(result));
   else
      return lower_type(result);
}

static vcode_cc_t lower_cc_for_call(tree_t call)
{
   tree_t decl = tree_ref(call);
   const subprogram_kind_t skind = tree_subkind(decl);

   if (skind == S_FOREIGN)
      return VCODE_CC_FOREIGN;
   else if (tree_flags(decl) & TREE_F_FOREIGN)
      return VCODE_CC_FOREIGN;
   else if (is_builtin(skind))
      return VCODE_CC_PREDEF;
   else
      return VCODE_CC_VHDL;
}

static vcode_reg_t lower_context_for_call(ident_t unit_name)
{
   ident_t scope_name = ident_runtil(ident_runtil(unit_name, '('), '.');

   if (vcode_unit_kind() == VCODE_UNIT_THUNK) {
      // This is a hack to make thunks work
      tree_t pack = lib_get_qualified(scope_name);
      if (pack != NULL && tree_kind(pack) == T_PACKAGE)
         return emit_link_package(scope_name);
      else
         return emit_null(vtype_context(scope_name));
   }

   vcode_state_t state;
   vcode_state_save(&state);

   vcode_unit_t vu = vcode_find_unit(unit_name);
   if (vu != NULL) {
      vcode_select_unit(vu);
      if (vcode_unit_kind() != VCODE_UNIT_THUNK) {
         vcode_select_unit(vcode_unit_context());
         if (vcode_unit_kind() != VCODE_UNIT_THUNK) {
            scope_name = vcode_unit_name();
         }
      }
   }

   vcode_state_restore(&state);

   int hops = 0;
   for (; vcode_unit_name() != scope_name; hops++) {
      vcode_unit_t context = vcode_unit_context();
      if (context == NULL) {
         vcode_state_restore(&state);
         if (ident_until(scope_name, '-') != scope_name) {
            // Call to function defined in architecture
            return emit_null(vtype_context(scope_name));
         }
         else
            return emit_link_package(scope_name);
      }
      else
         vcode_select_unit(context);
   }

   vcode_state_restore(&state);

   return emit_context_upref(hops);
}

static vcode_reg_t lower_fcall(tree_t fcall, expr_ctx_t ctx)
{
   tree_t decl = tree_ref(fcall);

   const subprogram_kind_t kind = tree_subkind(decl);
   if (is_open_coded_builtin(kind))
      return lower_builtin(fcall, kind);

   const int nparams = tree_params(fcall);
   SCOPED_A(vcode_reg_t) args = AINIT;

   const vcode_cc_t cc = lower_cc_for_call(fcall);
   ident_t name = tree_ident2(decl);

   if (tree_kind(fcall) == T_PROT_FCALL && tree_has_name(fcall))
      APUSH(args, lower_reify(lower_expr(tree_name(fcall), EXPR_RVALUE)));
   else if (cc != VCODE_CC_FOREIGN)
      APUSH(args, lower_context_for_call(name));

   for (int i = 0; i < nparams; i++)
      APUSH(args, lower_subprogram_arg(fcall, i));

   type_t result = type_result(tree_type(decl));
   vcode_type_t rtype = lower_func_result_type(result);
   vcode_type_t rbounds = lower_bounds(result);
   return emit_fcall(name, rtype, rbounds, cc, args.items, args.count);
}

static vcode_reg_t *lower_string_literal_chars(tree_t lit, int *nchars)
{
   type_t ltype = tree_type(lit);
   vcode_type_t vtype = lower_type(type_elem(ltype));

   *nchars = tree_chars(lit);
   vcode_reg_t *tmp = xmalloc_array(*nchars, sizeof(vcode_reg_t));

   for (int i = 0; i < *nchars; i++)
      tmp[i] = emit_const(vtype, tree_pos(tree_ref(tree_char(lit, i))));

   return tmp;
}

static vcode_reg_t lower_string_literal(tree_t lit)
{
   int nchars;
   vcode_reg_t *tmp LOCAL = lower_string_literal_chars(lit, &nchars);

   type_t type = tree_type(lit);
   if (type_is_array(type) && !lower_const_bounds(type)) {
      vcode_type_t elem = lower_type(type_elem(type));
      vcode_type_t array_type = vtype_carray(nchars, elem, elem);
      vcode_reg_t data = emit_const_array(array_type, tmp, nchars);
      if (type_is_unconstrained(type)) {
         // Will occur with overridden generic strings
         vcode_dim_t dim0 = {
            .left  = emit_const(vtype_offset(), 1),
            .right = emit_const(vtype_offset(), nchars),
            .dir   = emit_const(vtype_bool(), RANGE_TO)
         };
         return emit_wrap(emit_address_of(data), &dim0, 1);
      }
      else
         return lower_wrap(type, emit_address_of(data));
   }
   else
      return emit_const_array(lower_type(type), tmp, nchars);
}

static vcode_reg_t lower_literal(tree_t lit, expr_ctx_t ctx)
{
   if (ctx == EXPR_LVALUE)
      return VCODE_INVALID_REG;

   switch (tree_subkind(lit)) {
   case L_PHYSICAL:
      assert(!tree_has_ref(lit));
      // Fall-through
   case L_INT:
      return emit_const(lower_type(tree_type(lit)), tree_ival(lit));

   case L_STRING:
      {
         vcode_reg_t array = lower_string_literal(lit);
         if (vcode_reg_kind(array) == VCODE_TYPE_CARRAY)
            array = emit_address_of(array);
         return array;
      }

   case L_NULL:
      return emit_null(lower_type(tree_type(lit)));

   case L_REAL:
      return emit_const_real(tree_dval(lit));

   default:
      fatal_at(tree_loc(lit), "cannot lower literal kind %d",
               tree_subkind(lit));
   }
}

static void lower_push_scope(tree_t container)
{
   lower_scope_t *new = xcalloc(sizeof(lower_scope_t));
   new->down      = top_scope;
   new->objects   = hash_new(128, true);
   new->container = container;

   top_scope = new;
}

static void lower_pop_scope(void)
{
   lower_scope_t *tmp = top_scope;
   top_scope = tmp->down;
   hash_free(tmp->objects);
   free(tmp);
}

static int lower_search_vcode_obj(void *key, lower_scope_t *scope, int *hops)
{
   *hops = 0;
   for (; scope != NULL; scope = scope->down) {
      const void *ptr = hash_get(scope->objects, key);
      const int obj = (uintptr_t)ptr - 1;
      if (obj != VCODE_INVALID_REG)
         return obj;
      else if (scope->container != NULL
               && tree_kind(scope->container) == T_PROT_BODY)
         continue;
      (*hops)++;
   }

   *hops = 0;
   return VCODE_INVALID_REG;
}

static void lower_put_vcode_obj(void *key, int obj, lower_scope_t *scope)
{
   hash_put(scope->objects, key, (void *)(uintptr_t)(obj + 1));
}

static vcode_var_t lower_get_var(tree_t decl, int *hops)
{
   return lower_search_vcode_obj(decl, top_scope, hops);
}

static vcode_reg_t lower_link_var(tree_t decl)
{
   type_t type = tree_type(decl);
   ident_t name = tree_ident2(decl);

   vcode_type_t vtype;
   if (class_of(decl) == C_SIGNAL)
      vtype = lower_signal_type(type);
   else if (type_is_array(type) && lower_const_bounds(type))
      vtype = lower_type(lower_elem_recur(type));
   else
      vtype = lower_type(type);

   return emit_link_var(name, vtype);
}

static vcode_reg_t lower_var_ref(tree_t decl, expr_ctx_t ctx)
{
   type_t type = tree_type(decl);

   vcode_reg_t ptr_reg = VCODE_INVALID_REG;
   int hops = 0;
   vcode_var_t var = lower_get_var(decl, &hops);
   if (var == VCODE_INVALID_VAR) {
      if (mode == LOWER_THUNK) {
         if (tree_kind(decl) == T_CONST_DECL) {
            if (tree_has_value(decl)) {
               tree_t value = tree_value(decl);
               vcode_reg_t reg = lower_expr(value, ctx);
               if (type_is_array(type))
                  return lower_coerce_arrays(tree_type(value), type, reg);
               else
                  return reg;
            }
            else
               ptr_reg = lower_link_var(decl);  // External constant
         }
         else {
            emit_comment("Cannot resolve variable %s", istr(tree_ident(decl)));
            vcode_type_t vtype = lower_type(type);
            const vtype_kind_t vtkind = vtype_kind(vtype);
            if (vtkind == VCODE_TYPE_CARRAY)
               return emit_undefined(vtype_pointer(vtype_elem(vtype)));
            else if (ctx == EXPR_LVALUE || vtkind == VCODE_TYPE_RECORD)
               return emit_undefined(vtype_pointer(vtype));
            else
               return emit_undefined(vtype);
         }
      }
      else
         ptr_reg = lower_link_var(decl);   // External variable
   }
   else if (hops > 0)
      ptr_reg = emit_var_upref(hops, var);

   if (ptr_reg != VCODE_INVALID_REG) {
      if (ctx == EXPR_LVALUE)
         return ptr_reg;
      else if (type_is_scalar(type))
         return emit_load_indirect(ptr_reg);
      else if (type_is_array(type) && !lower_const_bounds(type))
         return emit_load_indirect(ptr_reg);
      else
         return ptr_reg;
   }
   else if (type_is_array(type) && lower_const_bounds(type))
      return emit_index(var, VCODE_INVALID_REG);
   else if (type_is_record(type) || type_is_protected(type))
      return emit_index(var, VCODE_INVALID_REG);
   else if ((type_is_scalar(type) || type_is_file(type) || type_is_access(type))
            && ctx == EXPR_LVALUE)
      return emit_index(var, VCODE_INVALID_REG);
   else
      return emit_load(var);
}

static vcode_reg_t lower_signal_ref(tree_t decl, expr_ctx_t ctx)
{
   type_t type = tree_type(decl);

   if (mode == LOWER_THUNK)
      return emit_undefined(lower_signal_type(type));

   int hops = 0;
   vcode_var_t var = lower_search_vcode_obj(decl, top_scope, &hops);

   vcode_reg_t sig_reg;
   if (var == VCODE_INVALID_VAR) {
      // Link to external package signal
      sig_reg = emit_load_indirect(lower_link_var(decl));
   }
   else if (hops == 0)
      sig_reg = emit_load(var);
   else
      sig_reg = emit_load_indirect(emit_var_upref(hops, var));

   if (ctx == EXPR_RVALUE)
      return emit_resolved(lower_array_data(sig_reg));
   else
      return sig_reg;
}

static vcode_reg_t lower_param_ref(tree_t decl, expr_ctx_t ctx)
{
   int hops = 0;
   int obj = lower_search_vcode_obj(decl, top_scope, &hops);

   // TODO: remove this....
   const bool is_entity_port =
      obj != VCODE_INVALID_VAR && !!(obj & 0x80000000);
   const bool is_generic =
      obj != VCODE_INVALID_VAR && !!(obj & 0x40000000);
   const bool is_proc_var =
      obj != VCODE_INVALID_VAR && !!(obj & 0x20000000);

   if (is_entity_port) {
      if (ctx != EXPR_LVALUE && tree_subkind(decl) == PORT_INOUT) {
         // Actually we wanted to get the input aspect ($in suffix)
         void *key = (void *)((uintptr_t)decl | 1);
         obj = lower_search_vcode_obj(key, top_scope, &hops);
      }

      if (mode == LOWER_THUNK) {
         emit_comment("Cannot resolve reference to signal %s in thunk",
                      istr(tree_ident(decl)));
         return emit_undefined(lower_signal_type(tree_type(decl)));
      }
      else if (obj == VCODE_INVALID_VAR) {
         vcode_dump();
         fatal_trace("missing var for port %s", istr(tree_ident(decl)));
      }

      vcode_var_t var = obj & 0x7fffffff;
      vcode_reg_t sig_reg;
      if (hops == 0)
         sig_reg = emit_load(var);
      else
         sig_reg = emit_load_indirect(emit_var_upref(hops, var));

      if (ctx == EXPR_RVALUE)
         return emit_resolved(lower_array_data(sig_reg));
      else
         return sig_reg;
   }
   else if (is_generic) {
      type_t type = tree_type(decl);
      vcode_var_t var = obj & 0x3fffffff;
      if (hops > 0) {
         vcode_reg_t ptr_reg = emit_var_upref(hops, var);
         if (type_is_scalar(type))
            return emit_load_indirect(ptr_reg);
         else if (type_is_array(type) && !lower_const_bounds(type))
            return emit_load_indirect(ptr_reg);
         else
            return ptr_reg;
      }
      else if (type_is_array(type) && lower_const_bounds(type))
         return emit_index(var, VCODE_INVALID_REG);
      else if (type_is_record(type) || type_is_protected(type))
         return emit_index(var, VCODE_INVALID_REG);
      else
         return emit_load(var);
   }
   else if (hops > 0) {
      // Reference to parameter in parent subprogram
      return emit_load_indirect(emit_var_upref(hops, obj & 0x1fffffff));
   }
   else if (is_proc_var) {
      vcode_var_t var = obj & 0x1fffffff;
      return emit_load(var);
   }
   else {
      vcode_reg_t reg = obj;
      const bool undefined_in_thunk =
         mode == LOWER_THUNK && (reg == VCODE_INVALID_REG
                                 || tree_class(decl) == C_SIGNAL
                                 || type_is_protected(tree_type(decl)));
      if (undefined_in_thunk) {
         emit_comment("Cannot resolve reference to %s", istr(tree_ident(decl)));
         if (tree_class(decl) == C_SIGNAL)
            return emit_undefined(lower_signal_type(tree_type(decl)));
         else {
            vcode_type_t vtype = lower_type(tree_type(decl));
            if (vtype_kind(vtype) == VCODE_TYPE_RECORD)
               return emit_undefined(vtype_pointer(vtype));
            else
               return emit_undefined(vtype);
         }
      }
      else if (reg == VCODE_INVALID_REG
               && vcode_unit_kind() == VCODE_UNIT_INSTANCE
               && tree_class(decl) == C_CONSTANT) {
         // This can happen when a type contains a reference to a
         // component generic. The elaborator does not currently rewrite
         // it to point at the corresponding entity generic.

         vcode_var_t var = vcode_find_var(tree_ident(decl));
         assert(var != VCODE_INVALID_VAR);

         type_t type = tree_type(decl);
         if (type_is_array(type) && lower_const_bounds(type))
            return emit_index(var, VCODE_INVALID_REG);
         else if (type_is_record(type) || type_is_protected(type))
            return emit_index(var, VCODE_INVALID_REG);
         else
            return emit_load(var);
      }
      else if (reg == VCODE_INVALID_REG) {
         vcode_dump();
         fatal_trace("missing register for parameter %s",
                     istr(tree_ident(decl)));
      }

      return reg;
   }
}

static vcode_reg_t lower_alias_ref(tree_t alias, expr_ctx_t ctx)
{
   tree_t value = tree_value(alias);
   type_t type = tree_type(value);

   if (!type_is_array(type))
      return lower_expr(tree_value(alias), ctx);

   int hops = 0;
   vcode_var_t var = lower_get_var(alias, &hops);
   if (var == VCODE_INVALID_VAR) {
      if (mode == LOWER_THUNK)
         return emit_undefined(lower_type(type));
      else {
         // External alias variable
         return emit_load_indirect(lower_link_var(alias));
      }
   }

   vcode_state_t state;
   vcode_state_save(&state);

   for (int i = 0; i < hops; i++)
      vcode_select_unit(vcode_unit_context());

   vcode_state_restore(&state);

   if (hops == 0)
      return emit_load(var);
   else
      return emit_load_indirect(emit_var_upref(hops, var));
}

static bool lower_is_trivial_constant(tree_t decl)
{
   if (!type_is_scalar(tree_type(decl)))
      return false;
   else if (!tree_has_value(decl))
      return false;
   else
      return tree_kind(tree_value(decl)) == T_LITERAL;
}

static vcode_reg_t lower_ref(tree_t ref, expr_ctx_t ctx)
{
   tree_t decl = tree_ref(ref);

   tree_kind_t kind = tree_kind(decl);
   switch (kind) {
   case T_ENUM_LIT:
      if (ctx == EXPR_LVALUE)
         return VCODE_INVALID_REG;
      else
         return emit_const(lower_type(tree_type(decl)), tree_pos(decl));

   case T_VAR_DECL:
   case T_FILE_DECL:
      return lower_var_ref(decl, ctx);

   case T_PORT_DECL:
      return lower_param_ref(decl, ctx);

   case T_SIGNAL_DECL:
   case T_IMPLICIT_SIGNAL:
      return lower_signal_ref(decl, ctx);

   case T_TYPE_DECL:
      return VCODE_INVALID_REG;

   case T_CONST_DECL:
      if (ctx == EXPR_LVALUE)
         return VCODE_INVALID_REG;
      else if (lower_is_trivial_constant(decl))
         return lower_expr(tree_value(decl), ctx);
      else
         return lower_var_ref(decl, ctx);

   case T_UNIT_DECL:
      return lower_expr(tree_value(decl), ctx);

   case T_ALIAS:
      return lower_alias_ref(decl, ctx);

   default:
      vcode_dump();
      fatal_trace("cannot lower reference to %s", tree_kind_str(kind));
   }
}

static vcode_reg_t lower_array_off(vcode_reg_t off, vcode_reg_t array,
                                   type_t type, unsigned dim)
{
   // Convert VHDL offset 'off' to a zero-based array offset

   assert(vtype_kind(vcode_reg_type(off)) == VCODE_TYPE_INT);

   const bool wrapped = vtype_kind(vcode_reg_type(array)) == VCODE_TYPE_UARRAY
      || type_is_unconstrained(type);

   vcode_reg_t zeroed = VCODE_INVALID_REG;
   if (wrapped) {
      vcode_reg_t meta_reg = lower_reify(array);
      vcode_reg_t left_reg = lower_array_left(type, dim, meta_reg);

      vcode_reg_t downto = emit_sub(left_reg, off);
      vcode_reg_t upto   = emit_sub(off, left_reg);
      zeroed = emit_select(emit_uarray_dir(meta_reg, dim), downto, upto);
   }
   else {
      tree_t r = range_of(type, dim);
      vcode_reg_t left = lower_reify_expr(tree_left(r));
      if (tree_subkind(r) == RANGE_TO)
         zeroed = emit_sub(off, left);
      else
         zeroed = emit_sub(left, off);
   }

   return emit_cast(vtype_offset(), VCODE_INVALID_TYPE, zeroed);
}

static void lower_check_array_bounds(type_t type, int dim, vcode_reg_t array,
                                     vcode_reg_t value, tree_t where,
                                     tree_t hint)
{
   PUSH_DEBUG_INFO(tree_kind(where) == T_PORT_DECL ? hint : where);

   vcode_reg_t left_reg  = lower_array_left(type, dim, array);
   vcode_reg_t right_reg = lower_array_right(type, dim, array);
   vcode_reg_t dir_reg   = lower_array_dir(type, dim, array);

   vcode_reg_t min_reg = emit_select(dir_reg, right_reg, left_reg);
   vcode_reg_t max_reg = emit_select(dir_reg, left_reg, right_reg);

   vcode_type_t kind_type = vtype_offset();
   vcode_reg_t to_reg = emit_const(kind_type, BOUNDS_ARRAY_TO);
   vcode_reg_t downto_reg = emit_const(kind_type, BOUNDS_ARRAY_DOWNTO);
   vcode_reg_t kind_reg = emit_select(dir_reg, downto_reg, to_reg);

   char *hint_str LOCAL = lower_get_hint_string(where, NULL);

   emit_dynamic_bounds(value, min_reg, max_reg, kind_reg, hint_str);
}

static vcode_reg_t lower_array_stride(vcode_reg_t array, type_t type)
{
   type_t elem = type_elem(type);
   if (type_is_array(elem)) {
      vcode_reg_t stride = lower_array_total_len(elem, VCODE_INVALID_REG);
      emit_comment("Array of array stride is r%d", stride);
      return stride;
   }
   else
      return emit_const(vtype_offset(), 1);
}

static vcode_reg_t lower_array_ref_offset(tree_t ref, vcode_reg_t array)
{
   tree_t value = tree_value(ref);
   type_t value_type = tree_type(value);

   const bool elide_bounds = tree_flags(ref) & TREE_F_ELIDE_BOUNDS;

   vcode_reg_t idx = emit_const(vtype_offset(), 0);
   const int nparams = tree_params(ref);
   for (int i = 0; i < nparams; i++) {
      tree_t p = tree_param(ref, i);
      assert(tree_subkind(p) == P_POS);

      vcode_reg_t offset = lower_reify_expr(tree_value(p));

      if (!elide_bounds)
         lower_check_array_bounds(value_type, i, array, offset,
                                  tree_value(p), NULL);

      if (i > 0) {
         vcode_reg_t stride = lower_array_len(value_type, i, array);
         idx = emit_mul(idx, stride);
      }

      idx = emit_add(idx, lower_array_off(offset, array, value_type, i));
   }

   idx = emit_mul(idx, lower_array_stride(array, value_type));

   return idx;
}

static vcode_reg_t lower_array_ref(tree_t ref, expr_ctx_t ctx)
{
   tree_t value = tree_value(ref);

   vcode_reg_t array = lower_expr(value, ctx);
   if (array == VCODE_INVALID_REG)
      return array;

   const vtype_kind_t vtkind = vtype_kind(vcode_reg_type(array));
   assert(vtkind == VCODE_TYPE_POINTER || vtkind == VCODE_TYPE_UARRAY
          || vtkind == VCODE_TYPE_SIGNAL);

   vcode_reg_t offset_reg = lower_array_ref_offset(ref, array);
   vcode_reg_t data_reg   = lower_array_data(array);
   return emit_add(data_reg, offset_reg);
}

static vcode_reg_t lower_array_slice(tree_t slice, expr_ctx_t ctx)
{
   tree_t value = tree_value(slice);
   tree_t r     = tree_range(slice, 0);
   type_t type  = tree_type(value);

   vcode_reg_t left_reg  = lower_range_left(r);
   vcode_reg_t right_reg = lower_range_right(r);
   vcode_reg_t kind_reg  = lower_range_dir(r);
   vcode_reg_t null_reg  = emit_range_null(left_reg, right_reg, kind_reg);
   vcode_reg_t array_reg = lower_expr(value, ctx);

   int64_t null_const;
   const bool known_not_null =
      vcode_reg_const(null_reg, &null_const) && null_const == 0;

   vcode_block_t after_bounds_bb = VCODE_INVALID_BLOCK;
   if (!known_not_null) {
      vcode_block_t not_null_bb = emit_block();
      after_bounds_bb = emit_block();
      emit_cond(null_reg, after_bounds_bb, not_null_bb);

      vcode_select_block(not_null_bb);
   }

   tree_t left, right;
   if (tree_subkind(r) == RANGE_EXPR)
      left = right = r;
   else {
      left  = tree_left(r);
      right = tree_right(r);
   }

   lower_check_array_bounds(type, 0, array_reg, left_reg, left, NULL);
   lower_check_array_bounds(type, 0, array_reg, right_reg, right, NULL);

   if (!known_not_null) {
      emit_jump(after_bounds_bb);
      vcode_select_block(after_bounds_bb);
   }

   if (array_reg == VCODE_INVALID_REG)
      return VCODE_INVALID_REG;

   vcode_reg_t stride_reg = lower_array_stride(array_reg, type);

   vcode_reg_t data_reg = lower_array_data(array_reg);
   vcode_reg_t off_reg  = lower_array_off(left_reg, array_reg, type, 0);
   vcode_reg_t ptr_reg  = emit_add(data_reg, emit_mul(off_reg, stride_reg));

   const bool unwrap = lower_is_const(left) && lower_is_const(right);

   if (unwrap)
      return ptr_reg;
   else {
      vcode_dim_t dim0 = {
         .left  = left_reg,
         .right = right_reg,
         .dir   = kind_reg
      };
      return emit_wrap(ptr_reg, &dim0, 1);
   }
}

static void lower_copy_vals(vcode_reg_t *dst, const vcode_reg_t *src,
                            unsigned n, bool backwards)
{
   while (n--) {
      *dst = *src;
      ++src;
      dst += (backwards ? -1 : 1);
   }
}

static vcode_reg_t *lower_const_array_aggregate(tree_t t, type_t type,
                                                int dim, int *n_elems)
{
   if ((*n_elems = lower_array_const_size(type)) == 0)
      return NULL;

   vcode_reg_t *vals = xmalloc_array(*n_elems, sizeof(vcode_reg_t));

   for (int i = 0; i < *n_elems; i++)
      vals[i] = VCODE_INVALID_VAR;

   tree_t r = range_of(type, dim);
   const int64_t left = assume_int(tree_left(r));
   const bool is_downto = (tree_subkind(r) == RANGE_DOWNTO);

   const int nassocs = tree_assocs(t);
   for (int i = 0; i < nassocs; i++) {
      tree_t a = tree_assoc(t, i);
      tree_t value = tree_value(a);

      const tree_kind_t value_kind = tree_kind(value);

      vcode_reg_t tmp = VCODE_INVALID_REG;
      vcode_reg_t *sub = &tmp;
      int nsub = 1;
      if (value_kind == T_AGGREGATE) {
         type_t sub_type = tree_type(value);
         if (type_is_array(sub_type))
            sub = lower_const_array_aggregate(value, sub_type, 0, &nsub);
         else if (type_is_record(sub_type))
            *sub = lower_record_aggregate(value, true,
                                          lower_is_const(value), EXPR_RVALUE);
         else
            assert(false);
      }
      else if (value_kind == T_LITERAL && tree_subkind(value) == L_STRING)
         sub = lower_string_literal_chars(value, &nsub);
      else
         *sub = lower_expr(value, EXPR_RVALUE);

      switch (tree_subkind(a)) {
      case A_POS:
         lower_copy_vals(vals + (i * nsub), sub, nsub, false);
         break;

      case A_NAMED:
         {
            const int64_t name = assume_int(tree_name(a));
            const int64_t off  = is_downto ? left - name : name - left;
            lower_copy_vals(vals + (off * nsub), sub, nsub, false);
         }
         break;

      case A_OTHERS:
         assert((*n_elems % nsub) == 0);
         for (int j = 0; j < (*n_elems / nsub); j++) {
            if (vals[j * nsub] == VCODE_INVALID_REG)
               lower_copy_vals(vals + (j * nsub), sub, nsub, false);
         }
         break;

      case A_RANGE:
         {
            int64_t r_low, r_high;
            range_bounds(tree_range(a, 0), &r_low, &r_high);

            for (int j = r_low; j <= r_high; j++) {
               const int64_t off = is_downto ? left - j : j - left;
               lower_copy_vals(vals + (off * nsub), sub, nsub, false);
            }
         }
         break;
      }

      if (sub != &tmp)
         free(sub);
   }

   for (int i = 0; i < *n_elems; i++)
      assert(vals[i] != VCODE_INVALID_VAR);

   return vals;
}

static int lower_bit_width(type_t type)
{
   switch (type_kind(type)) {
   case T_INTEGER:
   case T_PHYSICAL:
      {
         tree_t r = range_of(type, 0);
         return bits_for_range(assume_int(tree_left(r)),
                               assume_int(tree_right(r)));
      }

   case T_REAL:
       // All real types are doubles at the moment
       return 64;

   case T_SUBTYPE:
      return lower_bit_width(type_base(type));

   case T_ENUM:
      return bits_for_range(0, type_enum_literals(type) - 1);

   case T_ARRAY:
      return lower_bit_width(type_elem(type));

   default:
      fatal_trace("unhandled type %s in lower_bit_width", type_pp(type));
   }
}

static int lower_byte_width(type_t type)
{
   return (lower_bit_width(type) + 7) / 8;
}

static bool lower_memset_bit_pattern(tree_t value, unsigned bits, uint8_t *byte)
{
   // If a tree has a constant value and that value's bit pattern consists
   // of the same repeated byte then we can use memset to initialise an
   // array with this

   int64_t ival;
   if (!folded_int(value, &ival))
      return false;

   const unsigned bytes = (bits + 7) / 8;

   *byte = ival & 0xff;
   for (int i = 0; i < bytes; i++) {
      const uint8_t next = ival & 0xff;
      if (next != *byte)
         return false;

      ival >>= 8;
   }

   return true;
}

static vcode_reg_t lower_dyn_aggregate(tree_t agg, type_t type)
{
   type_t agg_type = tree_type(agg);
   type_t elem_type = type_elem(type);

   emit_comment("Begin dynamic aggregrate line %d", tree_loc(agg)->first_line);

   vcode_reg_t def_reg = VCODE_INVALID_REG;
   tree_t def_value = NULL;
   const int nassocs = tree_assocs(agg);
   for (int i = 0; def_value == NULL && i < nassocs; i++) {
      tree_t a = tree_assoc(agg, i);
      if (tree_subkind(a) == A_OTHERS) {
         def_value = tree_value(a);
         if (type_is_scalar(tree_type(def_value)))
            def_reg = lower_reify_expr(def_value);
      }
   }

   assert(!type_is_unconstrained(agg_type));

   vcode_reg_t dir_reg   = lower_array_dir(agg_type, 0, VCODE_INVALID_REG);
   vcode_reg_t left_reg  = lower_array_left(agg_type, 0, VCODE_INVALID_REG);
   vcode_reg_t right_reg = lower_array_right(agg_type, 0, VCODE_INVALID_REG);
   vcode_reg_t len_reg   = lower_array_total_len(agg_type, VCODE_INVALID_REG);

   type_t scalar_elem_type = lower_elem_recur(elem_type);

   const bool multidim =
      type_is_array(agg_type) && dimension_of(agg_type) > 1;

   vcode_reg_t mem_reg = emit_alloca(lower_type(scalar_elem_type),
                                     lower_bounds(scalar_elem_type), len_reg);

   vcode_type_t offset_type = vtype_offset();

   vcode_dim_t dim0 = {
      .left  = left_reg,
      .right = right_reg,
      .dir   = dir_reg
   };

   tree_t agg0 = tree_assoc(agg, 0);

   uint8_t byte = 0;
   unsigned bits = 0;
   const bool can_use_memset =
      (type_is_integer(elem_type) || type_is_enum(elem_type))
      && tree_assocs(agg) == 1
      && tree_subkind(agg0) == A_OTHERS
      && !multidim
      && ((bits = lower_bit_width(elem_type)) <= 8
          || lower_memset_bit_pattern(tree_value(agg0), bits, &byte));

   if (can_use_memset) {
      if (bits <= 8)
         emit_memset(mem_reg, lower_reify_expr(tree_value(agg0)), len_reg);
      else {
         vcode_reg_t byte_reg  = emit_const(vtype_int(0, 255), byte);
         emit_memset(mem_reg, byte_reg,
                     emit_mul(len_reg,
                              emit_const(offset_type, (bits + 7) / 8)));
      }

      return emit_wrap(mem_reg, &dim0, 1);
   }

   vcode_reg_t ivar = emit_alloca(offset_type, offset_type, VCODE_INVALID_REG);
   emit_store_indirect(emit_const(offset_type, 0), ivar);

   vcode_reg_t stride = VCODE_INVALID_REG;
   if (type_is_array(elem_type)) {
      stride = lower_array_total_len(elem_type, VCODE_INVALID_REG);
      emit_comment("Array of array stride is r%d", stride);
   }

   if (multidim) {
      if (stride == VCODE_INVALID_REG)
         stride = emit_const(vtype_offset(), 1);

      const int dims = dimension_of(agg_type);
      for (int i = 1; i < dims; i++)
         stride = emit_mul(stride,
                           lower_array_len(agg_type, i, VCODE_INVALID_REG));
      emit_comment("Multidimensional array stride is r%d", stride);
   }


   vcode_reg_t len0_reg = len_reg;
   if (type_is_array(elem_type) || multidim)
      len0_reg = lower_array_len(agg_type, 0, VCODE_INVALID_REG);

   vcode_block_t test_bb = emit_block();
   vcode_block_t body_bb = emit_block();
   vcode_block_t exit_bb = emit_block();

   emit_jump(test_bb);

   // Loop test
   vcode_select_block(test_bb);
   vcode_reg_t i_loaded = emit_load_indirect(ivar);
   vcode_reg_t ge = emit_cmp(VCODE_CMP_GEQ, i_loaded, len0_reg);
   emit_cond(ge, exit_bb, body_bb);

   // Loop body
   vcode_select_block(body_bb);

   // Regenerate non-scalar default values on each iteration
   if (def_reg == VCODE_INVALID_REG && def_value != NULL)
      def_reg = lower_expr(def_value, EXPR_RVALUE);

   vcode_reg_t what = def_reg;
   for (int i = 0; i < nassocs; i++) {
      tree_t a = tree_assoc(agg, i);

      assoc_kind_t kind = tree_subkind(a);
      vcode_reg_t value_reg = VCODE_INVALID_REG;
      if (kind != A_OTHERS) {
         tree_t value = tree_value(a);
         value_reg = lower_expr(value, EXPR_RVALUE);

         type_t value_type = tree_type(value);
         if (type_is_scalar(value_type))
            value_reg = lower_reify(value_reg);
         else if (type_is_array(value_type)) {
            if (!lower_const_bounds(value_type)) {
               lower_check_array_sizes(value, elem_type, value_type,
                                       VCODE_INVALID_REG, value_reg);
            }
            value_reg = lower_array_data(value_reg);
         }
      }

      if (what == VCODE_INVALID_REG) {
         what = value_reg;
         continue;
      }

      switch (kind) {
      case A_POS:
         {
            vcode_reg_t eq = emit_cmp(VCODE_CMP_EQ, i_loaded,
                                      emit_const(offset_type, tree_pos(a)));
            what = emit_select(eq, value_reg, what);
         }
         break;

      case A_NAMED:
         {
            vcode_reg_t name_reg   = lower_reify_expr(tree_name(a));
            vcode_reg_t downto_reg = emit_sub(left_reg, name_reg);
            vcode_reg_t upto_reg   = emit_sub(name_reg, left_reg);

            vcode_reg_t off_reg  = emit_select(dir_reg, downto_reg, upto_reg);
            vcode_reg_t cast_reg = emit_cast(vtype_offset(),
                                             VCODE_INVALID_TYPE, off_reg);

            vcode_reg_t eq = emit_cmp(VCODE_CMP_EQ, i_loaded, cast_reg);
            what = emit_select(eq, value_reg, what);
         }
         break;

      case A_RANGE:
         {
            tree_t r = tree_range(a, 0);
            const range_kind_t rkind = tree_subkind(r);

            vcode_cmp_t lpred =
               (rkind == RANGE_TO ? VCODE_CMP_GEQ : VCODE_CMP_LEQ);
            vcode_cmp_t rpred =
               (rkind == RANGE_TO ? VCODE_CMP_LEQ : VCODE_CMP_GEQ);

            vcode_reg_t lcmp_reg =
               emit_cmp(lpred, i_loaded,
                        emit_cast(offset_type, VCODE_INVALID_TYPE,
                                  lower_reify_expr(tree_left(r))));
            vcode_reg_t rcmp_reg =
               emit_cmp(rpred, i_loaded,
                        emit_cast(offset_type, VCODE_INVALID_TYPE,
                                  lower_reify_expr(tree_right(r))));

            vcode_reg_t in_reg = emit_or(lcmp_reg, rcmp_reg);
            what = emit_select(in_reg, value_reg, what);
         }
         break;

      case A_OTHERS:
         break;
      }
   }

   vcode_reg_t i_stride = i_loaded;
   if (stride != VCODE_INVALID_REG)
      i_stride = emit_mul(i_loaded, stride);

   vcode_reg_t ptr_reg = emit_add(mem_reg, i_stride);
   if (type_is_array(elem_type) || multidim) {
      vcode_reg_t src_reg = lower_array_data(what);
      emit_copy(ptr_reg, src_reg, stride);
   }
   else if (type_is_record(elem_type))
      emit_copy(ptr_reg, what, VCODE_INVALID_REG);
   else
      emit_store_indirect(lower_reify(what), ptr_reg);

   emit_store_indirect(emit_add(i_loaded, emit_const(vtype_offset(), 1)), ivar);
   emit_jump(test_bb);

   // Epilogue
   vcode_select_block(exit_bb);
   emit_comment("End dynamic aggregrate line %d", tree_loc(agg)->first_line);

   return emit_wrap(mem_reg, &dim0, 1);
}

static vcode_reg_t lower_record_sub_aggregate(tree_t value, type_t type,
                                              bool is_const, expr_ctx_t ctx)
{
   if (type_is_array(type) && is_const) {
      if (tree_kind(value) == T_LITERAL)
         return lower_string_literal(value);
      else if (mode == LOWER_THUNK && !lower_const_bounds(type))
         return emit_undefined(lower_type(type));
      else {
         int nvals;
         vcode_reg_t *values LOCAL =
            lower_const_array_aggregate(value, type, 0, &nvals);
         return emit_const_array(lower_type(type), values, nvals);
      }
   }
   else if (type_is_record(type) && is_const)
      return lower_record_aggregate(value, true, true, ctx);
   else if (type_is_scalar(type))
      return lower_reify_expr(value);
   else
      return lower_expr(value, ctx);
}

static vcode_reg_t lower_record_aggregate(tree_t expr, bool nest,
                                          bool is_const, expr_ctx_t ctx)
{
   type_t type = tree_type(expr);
   const int nfields = type_fields(type);
   const int nassocs = tree_assocs(expr);

   vcode_reg_t vals[nfields];
   for (int i = 0; i < nfields; i++)
      vals[i] = VCODE_INVALID_REG;

   for (int i = 0; i < nassocs; i++) {
      tree_t a = tree_assoc(expr, i);

      tree_t value = tree_value(a);
      type_t value_type = tree_type(value);

      switch (tree_subkind(a)) {
      case A_POS:
         vals[tree_pos(a)] =
            lower_record_sub_aggregate(value, value_type, is_const, ctx);
         break;

      case A_NAMED:
         {
            unsigned index = tree_pos(tree_ref(tree_name(a)));
            assert(index < nfields);
            vals[index] =
               lower_record_sub_aggregate(value, value_type, is_const, ctx);
         }
         break;

      case A_OTHERS:
         for (int j = 0; j < nfields; j++) {
            if (vals[j] == VCODE_INVALID_REG) {
               type_t ftype = tree_type(type_field(type, j));
               vals[j] = lower_record_sub_aggregate(value, ftype,
                                                    is_const, ctx);
            }
         }
         break;

      case A_RANGE:
         assert(false);
      }
   }

   for (int i = 0; i < nfields; i++)
      assert(vals[i] != VCODE_INVALID_REG);

   if (is_const) {
      vcode_reg_t reg = emit_const_record(lower_type(type), vals, nfields);
      if (!nest) {
         vcode_type_t vtype = lower_type(type);
         vcode_reg_t mem_reg = emit_alloca(vtype, vtype, VCODE_INVALID_REG);
         emit_copy(mem_reg, emit_address_of(reg), VCODE_INVALID_REG);
         return mem_reg;
      }
      else
         return reg;
   }
   else {
      vcode_type_t vtype = lower_type(type);
      vcode_reg_t mem_reg = emit_alloca(vtype, vtype, VCODE_INVALID_REG);

      for (int i = 0; i < nfields; i++) {
         type_t ftype = tree_type(type_field(type, i));
         vcode_reg_t ptr_reg = emit_record_ref(mem_reg, i);
         if (type_is_array(ftype)) {
            if (lower_const_bounds(ftype)) {
               vcode_reg_t src_reg = lower_array_data(vals[i]);
               vcode_reg_t length_reg = lower_array_total_len(ftype, vals[i]);
               emit_copy(ptr_reg, src_reg, length_reg);
            }
            else {
               vcode_reg_t src_reg = vals[i];
               if (vcode_reg_kind(src_reg) != VCODE_TYPE_UARRAY)
                  src_reg = lower_wrap(ftype, src_reg);
               emit_store_indirect(src_reg, ptr_reg);
            }
         }
         else if (type_is_record(ftype))
            emit_copy(ptr_reg, vals[i], VCODE_INVALID_REG);
         else
            emit_store_indirect(vals[i], ptr_reg);
      }

      return mem_reg;
   }
}

static bool lower_can_use_const_rep(tree_t expr, int *length, tree_t *elem)
{
   if (tree_kind(expr) != T_AGGREGATE)
      return false;

   type_t type = tree_type(expr);

   if (!lower_const_bounds(type))
      return false;

   tree_t a0 = tree_assoc(expr, 0);
   if (tree_subkind(a0) != A_OTHERS)
      return false;

   tree_t others = tree_value(a0);
   type_t elem_type = tree_type(others);

   if (type_is_array(elem_type)) {
      if (!lower_can_use_const_rep(others, length, elem))
         return false;
   }
   else if (type_is_scalar(elem_type))
      *elem = others;
   else
      return false;

   *length = lower_array_const_size(type);
   return true;
}

static vcode_reg_t lower_aggregate(tree_t expr, expr_ctx_t ctx)
{
   type_t type = tree_type(expr);

   if (type_is_record(type))
      return lower_record_aggregate(expr, false, lower_is_const(expr), ctx);

   assert(type_is_array(type));

   if (lower_const_bounds(type) && lower_is_const(expr)) {
      int rep_size = -1;
      tree_t rep_elem = NULL;
      if (lower_can_use_const_rep(expr, &rep_size, &rep_elem) && rep_size > 1) {
         vcode_reg_t elem_reg = lower_reify_expr(rep_elem);
         return emit_const_rep(lower_type(type), elem_reg, rep_size);
      }
      else {
         int nvals;
         vcode_reg_t *values LOCAL =
            lower_const_array_aggregate(expr, type, 0, &nvals);

         vcode_reg_t array = emit_const_array(lower_type(type), values, nvals);
         return emit_address_of(array);
      }
   }
   else
      return lower_dyn_aggregate(expr, type);
}

static vcode_reg_t lower_record_ref(tree_t expr, expr_ctx_t ctx)
{
   tree_t value = tree_value(expr);
   type_t type = tree_type(value);
   vcode_reg_t record = lower_expr(value, ctx);

   const int index = tree_pos(tree_ref(expr));
   type_t ftype = tree_type(type_field(type, index));

   if (lower_have_signal(record) && ctx == EXPR_RVALUE)
      return emit_record_ref(emit_resolved(record), index);
   else if (type_is_array(ftype) && !lower_const_bounds(ftype))
      return emit_load_indirect(emit_record_ref(record, index));
   else
      return emit_record_ref(record, index);
}

static vcode_reg_t lower_concat(tree_t expr, expr_ctx_t ctx)
{
   const int nparams = tree_params(expr);

   struct {
      tree_t      value;
      type_t      type;
      vcode_reg_t reg;
   } args[nparams];

   for (int i = 0; i < nparams; i++) {
      args[i].value = tree_value(tree_param(expr, i));
      args[i].type  = tree_type(args[i].value);
      args[i].reg   = lower_expr(args[i].value, ctx);
   }

   type_t type = tree_type(expr);
   type_t elem = type_elem(type);

   type_t scalar_elem = lower_elem_recur(elem);

   vcode_reg_t var_reg = VCODE_INVALID_REG;
   if (type_is_unconstrained(type)) {
      vcode_reg_t len   = emit_const(vtype_offset(), 0);
      vcode_reg_t right = emit_const(vtype_offset(), 0);
      for (int i = 0; i < nparams; i++) {
         vcode_reg_t len_i, right_i;
         if (type_is_array(args[i].type) && type_eq(args[i].type, type)) {
            len_i   = lower_array_total_len(args[i].type, args[i].reg);
            right_i = lower_array_len(args[i].type, 0, args[i].reg);
         }
         else
            len_i = right_i = emit_const(vtype_offset(), 1);

         len = emit_add(len, len_i);
         right = emit_add(right, right_i);
      }

      vcode_reg_t data = emit_alloca(lower_type(scalar_elem), lower_bounds(scalar_elem), len);

      vcode_dim_t dims[1] = {
         {
            .left  = emit_const(vtype_offset(), 1),
            .right = right,
            .dir   = emit_const(vtype_bool(), RANGE_TO)
         }
      };
      var_reg = emit_wrap(data, dims, 1);
   }
   else
      var_reg = emit_alloca(lower_type(scalar_elem),
                            lower_bounds(scalar_elem),
                            lower_array_total_len(type, VCODE_INVALID_REG));

   vcode_reg_t ptr = lower_array_data(var_reg);

   for (int i = 0; i < nparams; i++) {
      if (type_is_array(args[i].type)) {
         vcode_reg_t src_len =
            lower_array_total_len(args[i].type, args[i].reg);

         if (lower_have_signal(args[i].reg)) {
            vcode_reg_t data = lower_array_data(args[i].reg);
            args[i].reg = emit_resolved(data);
         }

         emit_copy(ptr, lower_array_data(args[i].reg), src_len);
         if (i + 1 < nparams)
            ptr = emit_add(ptr, src_len);
      }
      else if (type_is_record(args[i].type)) {
         emit_copy(ptr, args[i].reg, VCODE_INVALID_REG);
         if (i + 1 < nparams)
            ptr = emit_add(ptr, emit_const(vtype_offset(), 1));
      }
      else {
         emit_store_indirect(lower_reify(args[i].reg), ptr);
         if (i + 1 < nparams)
            ptr = emit_add(ptr, emit_const(vtype_offset(), 1));
      }
   }

   return var_reg;
}

static vcode_reg_t lower_new(tree_t expr, expr_ctx_t ctx)
{
   tree_t value = tree_value(expr);
   type_t value_type = tree_type(value);

   if (type_is_array(value_type)) {
      vcode_reg_t init_reg = lower_expr(value, EXPR_RVALUE);
      vcode_reg_t length_reg = lower_array_total_len(value_type, init_reg);

      type_t elem_type = lower_elem_recur(value_type);
      vcode_reg_t mem_reg = emit_new(lower_type(elem_type), length_reg);
      vcode_reg_t raw_reg = emit_all(mem_reg);

      emit_copy(raw_reg, lower_array_data(init_reg), length_reg);

      type_t result_type = type_access(tree_type(expr));
      if (!lower_const_bounds(result_type)) {
          // Need to allocate memory for both the array and its metadata
         vcode_reg_t meta_reg =
            lower_wrap_with_new_bounds(value_type, init_reg, raw_reg);
         vcode_reg_t result_reg =
            emit_new(lower_type(result_type), VCODE_INVALID_REG);
         emit_store_indirect(meta_reg, emit_all(result_reg));
         return result_reg;
      }
      else
         return mem_reg;
   }
   else if (type_is_record(value_type)) {
      vcode_reg_t result_reg =
         emit_new(lower_type(value_type), VCODE_INVALID_REG);
      vcode_reg_t all_reg = emit_all(result_reg);

      uint32_t hint = emit_storage_hint(all_reg, VCODE_INVALID_REG);
      vcode_reg_t init_reg = lower_expr(value, EXPR_RVALUE);
      vcode_clear_storage_hint(hint);

      emit_copy(all_reg, init_reg, VCODE_INVALID_REG);

      return result_reg;
   }
   else {
      vcode_reg_t result_reg =
         emit_new(lower_type(value_type), VCODE_INVALID_REG);
      vcode_reg_t all_reg = emit_all(result_reg);

      vcode_reg_t init_reg = lower_expr(value, EXPR_RVALUE);
      emit_store_indirect(lower_reify(init_reg), all_reg);

      return result_reg;
   }
}

static vcode_reg_t lower_incomplete_access(vcode_reg_t in_reg, type_t type)
{
   assert(vcode_reg_kind(in_reg) == VCODE_TYPE_ACCESS);

   vcode_type_t pointed = vtype_pointed(vcode_reg_type(in_reg));

   const bool need_cast =
      (type_is_incomplete(type) && vtype_kind(pointed) != VCODE_TYPE_OPAQUE)
      || (!type_is_incomplete(type)
          && vtype_kind(pointed) == VCODE_TYPE_OPAQUE);

   if (need_cast) {
      vcode_type_t ptr_type = vtype_access(lower_type(type));
      return emit_cast(ptr_type, ptr_type, in_reg);
   }

   return in_reg;
}

static vcode_reg_t lower_all(tree_t all, expr_ctx_t ctx)
{
   type_t type = tree_type(all);
   vcode_reg_t access_reg = lower_reify_expr(tree_value(all));
   emit_null_check(access_reg);
   access_reg = lower_incomplete_access(access_reg, tree_type(all));
   vcode_reg_t all_reg = emit_all(access_reg);

   if (type_is_array(type) && !lower_const_bounds(type))
      return lower_reify(all_reg);
   else
      return all_reg;
}

static vcode_reg_t lower_conversion(vcode_reg_t value_reg, tree_t where,
                                    type_t from, type_t to)
{
   type_kind_t from_k = type_kind(type_base_recur(from));
   type_kind_t to_k   = type_kind(type_base_recur(to));

   if (from_k == T_REAL && to_k == T_INTEGER) {
      vcode_reg_t scalar_reg = lower_reify(value_reg);
      vcode_type_t to_vtype = lower_type(to);
      vcode_reg_t cast = emit_cast(to_vtype, to_vtype, scalar_reg);
      lower_check_scalar_bounds(cast, to, where, NULL);
      return cast;
   }
   else if (from_k == T_INTEGER && to_k == T_REAL) {
      vcode_reg_t scalar_reg = lower_reify(value_reg);
      return emit_cast(lower_type(to), lower_bounds(to), scalar_reg);
   }
   else if (type_is_array(to) && !lower_const_bounds(to)) {
      // Need to wrap in metadata
      return lower_wrap(from, value_reg);
   }
   else if (from_k == T_INTEGER && to_k == T_INTEGER) {
      // Possibly change width
      value_reg = lower_reify(value_reg);
      lower_check_scalar_bounds(value_reg, to, where, NULL);
      return emit_cast(lower_type(to), lower_bounds(to), value_reg);
   }
   else {
      // No conversion to perform
      return value_reg;
   }
}

static vcode_reg_t lower_type_conv(tree_t expr, expr_ctx_t ctx)
{
   tree_t value = tree_value(expr);

   type_t from = tree_type(value);
   type_t to   = tree_type(expr);

   vcode_reg_t value_reg = lower_expr(value, ctx);
   return lower_conversion(value_reg, expr, from, to);
}

static const int lower_get_attr_dimension(tree_t expr)
{
   if (tree_params(expr) > 0)
      return assume_int(tree_value(tree_param(expr, 0))) - 1;
   else
      return 0;
}

static vcode_reg_t lower_attr_ref(tree_t expr, expr_ctx_t ctx)
{
   tree_t name = tree_name(expr);

   const attr_kind_t predef = tree_subkind(expr);
   switch (predef) {
   case ATTR_LEFT:
   case ATTR_RIGHT:
      {
         const int dim = lower_get_attr_dimension(expr);

         type_t type = tree_type(name);
         if (type_is_unconstrained(type)) {
            vcode_reg_t array_reg = lower_expr(name, EXPR_RVALUE);
            if (predef == ATTR_LEFT)
               return lower_array_left(type, dim, array_reg);
            else
               return lower_array_right(type, dim, array_reg);
         }
         else {
            tree_t r = range_of(type, dim);
            if (predef == ATTR_LEFT)
               return lower_range_left(r);
            else
               return lower_range_right(r);
         }
      }

   case ATTR_LOW:
   case ATTR_HIGH:
      {
         const int dim = lower_get_attr_dimension(expr);

         vcode_reg_t left_reg  = VCODE_INVALID_REG;
         vcode_reg_t right_reg = VCODE_INVALID_REG;
         vcode_reg_t dir_reg   = VCODE_INVALID_REG;

         type_t type = tree_type(name);
         if (type_is_unconstrained(type)) {
            vcode_reg_t array_reg = lower_expr(name, EXPR_RVALUE);
            left_reg  = lower_array_left(type, dim, array_reg);
            right_reg = lower_array_right(type, dim, array_reg);
            dir_reg   = lower_array_dir(type, dim, array_reg);
         }
         else {
            tree_t r = range_of(type, dim);
            const range_kind_t rkind = tree_subkind(r);
            if (rkind == RANGE_TO) {
               return predef == ATTR_LOW
                  ? lower_range_left(r) : lower_range_right(r);
            }
            else if (rkind == RANGE_DOWNTO) {
               return predef == ATTR_LOW
                  ? lower_range_right(r) : lower_range_left(r);
            }

            left_reg  = lower_range_left(r);
            right_reg = lower_range_right(r);
            dir_reg   = lower_range_dir(r);
         }

         if (predef == ATTR_LOW)
            return emit_select(dir_reg, right_reg, left_reg);
         else
            return emit_select(dir_reg, left_reg, right_reg);
      }

   case ATTR_LENGTH:
      {
         const int dim = lower_get_attr_dimension(expr);
         return emit_cast(lower_type(tree_type(expr)),
                          VCODE_INVALID_TYPE,
                          lower_array_len(tree_type(name), dim,
                                          lower_param(name, NULL, PORT_IN)));
      }

   case ATTR_ASCENDING:
      {
         type_t type = tree_type(name);
         const int dim = lower_get_attr_dimension(expr);
         if (lower_const_bounds(type))
            return emit_const(vtype_bool(),
                              direction_of(type, dim) == RANGE_TO);
         else
            return emit_not(lower_array_dir(type, dim,
                                            lower_param(name, NULL, PORT_IN)));
      }

   case ATTR_LAST_EVENT:
   case ATTR_LAST_ACTIVE:
      {
         type_t name_type = tree_type(name);
         vcode_reg_t name_reg = lower_expr(name, EXPR_LVALUE);
         vcode_reg_t len_reg = VCODE_INVALID_REG;
         if (type_is_array(name_type)) {
            len_reg = lower_array_total_len(name_type, name_reg);
            name_reg = lower_array_data(name_reg);
         }

         if (predef == ATTR_LAST_EVENT)
            return emit_last_event(name_reg, len_reg);
         else if (predef == ATTR_LAST_ACTIVE)
            return emit_last_active(name_reg, len_reg);
         else
            return emit_driving_flag(name_reg, len_reg);
      }

   case ATTR_DRIVING_VALUE:
      {
         type_t name_type = tree_type(name);
         vcode_reg_t name_reg = lower_expr(name, EXPR_LVALUE);
         if (type_is_array(name_type)) {
            vcode_reg_t len_reg = lower_array_total_len(name_type, name_reg);
            vcode_reg_t ptr_reg = emit_driving_value(name_reg, len_reg);
            if (lower_const_bounds(name_type))
               return ptr_reg;
            else
               return lower_wrap(name_type, ptr_reg);
         }
         else {
            vcode_reg_t ptr_reg =
               emit_driving_value(name_reg, VCODE_INVALID_REG);
            return emit_load_indirect(ptr_reg);
         }
      }

   case ATTR_EVENT:
      return lower_signal_flag(name, emit_event_flag);

   case ATTR_ACTIVE:
      return lower_signal_flag(name, emit_active_flag);

   case ATTR_DRIVING:
      return lower_signal_flag(name, emit_driving_flag);

   case ATTR_LAST_VALUE:
      return lower_last_value(name);

   case ATTR_INSTANCE_NAME:
   case ATTR_PATH_NAME:
   case ATTR_SIMPLE_NAME:
      return lower_name_attr(name, predef);

   case ATTR_IMAGE:
      {
         tree_t value = tree_value(tree_param(expr, 0));
         type_t base = type_base_recur(tree_type(value));
         ident_t func = ident_prefix(type_ident(base), ident_new("image"), '$');
         vcode_type_t ctype = vtype_char();
         vcode_type_t strtype = vtype_uarray(1, ctype, ctype);
         vcode_reg_t args[] = {
            lower_context_for_call(func),
            lower_param(value, NULL, PORT_IN)
         };
         return emit_fcall(func, strtype, strtype, VCODE_CC_PREDEF, args, 2);
      }

   case ATTR_VALUE:
      {
         type_t name_type = tree_type(name);
         tree_t value = tree_value(tree_param(expr, 0));
         type_t value_type = tree_type(value);

         vcode_reg_t arg_reg = lower_param(value, NULL, PORT_IN);
         if (lower_const_bounds(value_type))
            arg_reg = lower_wrap(value_type, arg_reg);

         type_t base = type_base_recur(name_type);
         ident_t func = ident_prefix(type_ident(base), ident_new("value"), '$');
         vcode_reg_t args[] = {
            lower_context_for_call(func),
            arg_reg
         };
         vcode_reg_t reg = emit_fcall(func, lower_type(base),
                                      lower_bounds(base),
                                      VCODE_CC_PREDEF, args, 2);
         lower_check_scalar_bounds(reg, name_type, expr, NULL);
         return emit_cast(lower_type(name_type), lower_bounds(name_type), reg);
      }

   case ATTR_SUCC:
      {
         tree_t value = tree_value(tree_param(expr, 0));
         vcode_reg_t arg = lower_param(value, NULL, PORT_IN);
         return emit_add(arg, emit_const(vcode_reg_type(arg), 1));
      }

   case ATTR_PRED:
      {
         tree_t value = tree_value(tree_param(expr, 0));
         vcode_reg_t arg = lower_param(value, NULL, PORT_IN);
         return emit_sub(arg, emit_const(vcode_reg_type(arg), 1));
      }

   case ATTR_LEFTOF:
      {
         tree_t value = tree_value(tree_param(expr, 0));
         vcode_reg_t arg = lower_param(value, NULL, PORT_IN);

         type_t type = tree_type(expr);
         const int dir =
            (type_is_enum(type) || direction_of(type, 0) == RANGE_TO) ? -1 : 1;
         return emit_add(arg, emit_const(vcode_reg_type(arg), dir));
      }

   case ATTR_RIGHTOF:
      {
         tree_t value = tree_value(tree_param(expr, 0));
         vcode_reg_t arg = lower_param(value, NULL, PORT_IN);

         type_t type = tree_type(expr);
         const int dir =
            (type_is_enum(type) || direction_of(type, 0) == RANGE_TO) ? 1 : -1;
         return emit_add(arg, emit_const(vcode_reg_type(arg), dir));
      }

   case ATTR_POS:
      {
         tree_t value = tree_value(tree_param(expr, 0));
         vcode_reg_t arg = lower_param(value, NULL, PORT_IN);

         type_t type = tree_type(expr);
         return emit_cast(lower_type(type), lower_bounds(type), arg);
      }

   case ATTR_VAL:
      {
         tree_t value = tree_value(tree_param(expr, 0));
         vcode_reg_t arg = lower_param(value, NULL, PORT_IN);

         type_t type = tree_type(expr);
         lower_check_scalar_bounds(arg, type, expr, NULL);
         return emit_cast(lower_type(type), lower_bounds(type), arg);
      }

   default:
      fatal_at(tree_loc(expr), "cannot lower attribute %s (%d)",
               istr(tree_ident(expr)), predef);
   }
}

static vcode_reg_t lower_qualified(tree_t expr, expr_ctx_t ctx)
{
   tree_t value = tree_value(expr);

   type_t from_type = tree_type(value);
   type_t to_type = tree_type(expr);

   vcode_reg_t value_reg = lower_expr(value, ctx);

   if (type_is_array(to_type)) {
      const bool from_const = lower_const_bounds(from_type);
      const bool to_const = lower_const_bounds(to_type);

      if (to_const && !from_const)
         return lower_array_data(value_reg);
      else if (!to_const && from_const)
         return lower_wrap(from_type, value_reg);

   }

   return value_reg;
}

static vcode_reg_t lower_expr(tree_t expr, expr_ctx_t ctx)
{
   PUSH_DEBUG_INFO(expr);

   switch (tree_kind(expr)) {
   case T_FCALL:
   case T_PROT_FCALL:
      return lower_fcall(expr, ctx);
   case T_LITERAL:
      return lower_literal(expr, ctx);
   case T_REF:
      return lower_ref(expr, ctx);
   case T_AGGREGATE:
      return lower_aggregate(expr, ctx);
   case T_ARRAY_REF:
      return lower_array_ref(expr, ctx);
   case T_ARRAY_SLICE:
      return lower_array_slice(expr, ctx);
   case T_RECORD_REF:
      return lower_record_ref(expr, ctx);
   case T_NEW:
      return lower_new(expr, ctx);
   case T_ALL:
      return lower_all(expr, ctx);
   case T_TYPE_CONV:
      return lower_type_conv(expr, ctx);
   case T_ATTR_REF:
      return lower_attr_ref(expr, ctx);
   case T_QUALIFIED:
      return lower_qualified(expr, ctx);
   case T_OPEN:
      return VCODE_INVALID_REG;
   default:
      fatal_at(tree_loc(expr), "cannot lower expression kind %s",
               tree_kind_str(tree_kind(expr)));
   }
}

static vcode_reg_t lower_default_value(type_t type, bool nested)
{
   if (type_is_scalar(type))
      return lower_range_left(range_of(type, 0));
   else if (type_is_array(type)) {
      vcode_reg_t elem_reg = lower_default_value(type_elem(type), true);
      if (lower_const_bounds(type)) {
         const int size = lower_array_const_size(type);
         vcode_reg_t *values LOCAL = xmalloc_array(size, sizeof(vcode_reg_t));
         for (int i = 0; i < size; i++)
            values[i] = elem_reg;
         vcode_reg_t cdata = emit_const_array(lower_type(type), values, size);
         return nested ? cdata : emit_address_of(cdata);
      }
      else
         fatal_at(tree_loc(range_of(type, 0)), "globally static bound of type "
                  "%s was not folded", type_pp(type));
   }
   else if (type_is_record(type)) {
      const int nfields = type_fields(type);
      vcode_reg_t *values LOCAL = xmalloc_array(nfields, sizeof(vcode_reg_t));
      for (int i = 0; i < nfields; i++)
         values[i] = lower_default_value(tree_type(type_field(type, i)), true);
      vcode_reg_t cdata = emit_const_record(lower_type(type), values, nfields);
      return nested ? cdata : emit_address_of(cdata);
   }
   else
      fatal_trace("cannot handle type %s in lower_default_value",
                  type_pp(type));
}

static void lower_assert(tree_t stmt)
{
   const int is_report = !tree_has_value(stmt);
   const int saved_mark = emit_temp_stack_mark();

   vcode_reg_t severity = lower_reify_expr(tree_severity(stmt));

   vcode_reg_t value = VCODE_INVALID_REG;
   if (!is_report) {
      value = lower_reify_expr(tree_value(stmt));

      int64_t value_const;
      if (vcode_reg_const(value, &value_const) && value_const)
         return;
   }

   vcode_block_t message_bb = VCODE_INVALID_BLOCK;
   vcode_block_t exit_bb = VCODE_INVALID_BLOCK;

   vcode_reg_t message = VCODE_INVALID_REG, length = VCODE_INVALID_REG;
   if (tree_has_message(stmt)) {
      tree_t m = tree_message(stmt);

      // If the message can have side effects then branch to a new block
      const tree_kind_t kind = tree_kind(m);
      const bool side_effects = kind != T_LITERAL;
      if (side_effects && !is_report) {
         message_bb = emit_block();
         exit_bb    = emit_block();
         emit_cond(value, exit_bb, message_bb);
         vcode_select_block(message_bb);
      }

      vcode_reg_t message_wrapped = lower_expr(m, EXPR_RVALUE);
      message = lower_array_data(message_wrapped);
      length  = lower_array_len(tree_type(m), 0, message_wrapped);
   }

   if (is_report)
      emit_report(message, length, severity);
   else
      emit_assert(value, message, length, severity);

   if (exit_bb != VCODE_INVALID_BLOCK) {
      emit_jump(exit_bb);
      vcode_select_block(exit_bb);
   }

   emit_temp_stack_restore(saved_mark);
}

static void lower_sched_event(tree_t on, bool is_static)
{
   tree_t ref = on, decl = NULL;
   while (decl == NULL) {
      switch (tree_kind(ref)) {
      case T_REF:
         decl = tree_ref(ref);
         break;

      case T_ARRAY_REF:
      case T_ARRAY_SLICE:
         ref = tree_value(ref);
         break;

      default:
         // It is possible for constant folding to replace a signal with
         // a constant which will then appear in a sensitivity list so
         // just ignore it
         return;
      }
   }

   tree_kind_t kind = tree_kind(decl);
   if (kind == T_ALIAS) {
      lower_sched_event(tree_value(decl), is_static);
      return;
   }
   else if (kind != T_SIGNAL_DECL && kind != T_PORT_DECL) {
      // As above, a port could have been rewritten to reference a
      // constant declaration or enumeration literal, in which case
      // just ignore it too
      return;
   }

   type_t type = tree_type(decl);
   type_t expr_type = tree_type(on);

   const bool array = type_is_array(type);

   vcode_reg_t n_elems = VCODE_INVALID_REG, nets = VCODE_INVALID_REG;
   if (tree_kind(on) == T_REF) {
      switch (tree_kind(decl)) {
      case T_SIGNAL_DECL:
         nets = lower_signal_ref(decl, EXPR_LVALUE);
         break;
      case T_PORT_DECL:
         if (tree_class(decl) != C_SIGNAL)
            return;
         nets = lower_param_ref(decl, EXPR_LVALUE);
         break;
      default:
         assert(false);
      }

      if (array) {
         type_t elem = type_elem(type);
         n_elems = lower_array_total_len(type, nets);
         if (type_is_record(elem))
            n_elems = emit_mul(n_elems,
                               emit_const(vtype_offset(), type_width(elem)));
      }
      else
         n_elems = emit_const(vtype_offset(), type_width(type));

      if (array && !lower_const_bounds(type)) {
         // Unwrap the meta-data to get nets array
         nets = emit_unwrap(nets);
      }
   }
   else {
      assert(array);
      nets = lower_expr(on, EXPR_LVALUE);

      if (type_is_array(expr_type))
         n_elems = lower_array_total_len(expr_type, VCODE_INVALID_REG);
      else
         n_elems = emit_const(vtype_offset(),1);
   }

   if (is_static)
      emit_sched_static(nets, n_elems);
   else
      emit_sched_event(nets, n_elems);
}

static void lower_wait(tree_t wait)
{
   const bool is_static = !!(tree_flags(wait) & TREE_F_STATIC_WAIT);
   assert(!is_static || (!tree_has_delay(wait) && !tree_has_value(wait)));

   if (!is_static) {
      // The _sched_event for static waits is emitted in the reset block
      const int ntriggers = tree_triggers(wait);
      for (int i = 0; i < ntriggers; i++)
         lower_sched_event(tree_trigger(wait, i), is_static);
   }

   const bool has_delay = tree_has_delay(wait);
   const bool has_value = tree_has_value(wait);

   vcode_reg_t delay = VCODE_INVALID_REG;
   if (has_delay)
      delay = lower_reify_expr(tree_delay(wait));

   vcode_var_t remain = VCODE_INVALID_VAR;
   if (has_value && has_delay) {
      ident_t remain_i = ident_new("wait_remain");
      remain = vcode_find_var(remain_i);
      if (remain == VCODE_INVALID_VAR) {
         vcode_type_t time = vtype_time();
         remain = emit_var(time, time, remain_i, 0);
      }

      vcode_type_t rtype = vtype_time();
      vcode_reg_t now_reg = emit_fcall(ident_new("_std_standard_now"),
                                       rtype, rtype, VCODE_CC_FOREIGN, NULL, 0);
      vcode_reg_t abs_reg = emit_add(now_reg, delay);
      emit_store(abs_reg, remain);
   }

   vcode_block_t resume = emit_block();
   emit_wait(resume, delay);

   vcode_select_block(resume);

   if (has_value) {
      // Generate code to loop until condition is met

      vcode_reg_t until_reg = lower_reify_expr(tree_value(wait));

      vcode_reg_t timeout_reg = VCODE_INVALID_REG;
      vcode_reg_t done_reg = until_reg;
      if (has_delay) {
         vcode_type_t rtype = vtype_time();
         vcode_reg_t remain_reg = emit_load(remain);
         vcode_reg_t now_reg = emit_fcall(ident_new("_std_standard_now"),
                                          rtype, rtype, VCODE_CC_FOREIGN,
                                          NULL, 0);
         timeout_reg = emit_sub(remain_reg, now_reg);

         vcode_reg_t expired_reg = emit_cmp(VCODE_CMP_EQ, timeout_reg,
                                            emit_const(vtype_time(), 0));
         done_reg = emit_or(expired_reg, until_reg);
      }

      vcode_block_t done_bb  = emit_block();
      vcode_block_t again_bb = emit_block();

      emit_cond(done_reg, done_bb, again_bb);

      vcode_select_block(again_bb);

      assert(!is_static);
      const int ntriggers = tree_triggers(wait);
      for (int i = 0; i < ntriggers; i++)
         lower_sched_event(tree_trigger(wait, i), is_static);

      emit_wait(resume, timeout_reg);

      vcode_select_block(done_bb);
   }
}

static void lower_check_array_sizes(tree_t where, type_t ltype, type_t rtype,
                                    vcode_reg_t lval, vcode_reg_t rval)
{
   const int ndims = dimension_of(ltype);
   for (int i = 0; i < ndims; i++) {
      vcode_reg_t llen_reg = lower_array_len(ltype, i, lval);
      vcode_reg_t rlen_reg = lower_array_len(rtype, i, rval);

      bounds_kind_t kind = BOUNDS_ARRAY_SIZE;
      char *hint_str LOCAL = NULL;

      if (where != NULL) {
         char *prefix LOCAL =
            ndims > 1 ? xasprintf(" for dimension %d", i + 1) : NULL;
         hint_str = lower_get_hint_string(where, prefix);
         if (tree_kind(where) == T_PORT_DECL)
            kind = BOUNDS_PARAM_SIZE;
      }

      emit_array_size(llen_reg, rlen_reg, kind, hint_str);
   }
}

static void lower_find_matching_refs(tree_t ref, void *context)
{
   tree_t *decl = (tree_t *)context;
   if (tree_ref(ref) == *decl)
      *decl = NULL;
}

static bool lower_assign_can_use_storage_hint(tree_t stmt)
{
   // We cannot write directly into an array variable's storage if the RHS
   // references that array

   tree_t target = tree_target(stmt);

   tree_kind_t kind;
   while ((kind = tree_kind(target)) != T_REF) {
      switch (kind) {
      case T_ARRAY_REF:
      case T_ARRAY_SLICE:
         target = tree_value(target);
         break;

      default:
         return false;
      }
   }

   tree_t decl = tree_ref(target);
   tree_visit_only(tree_value(stmt), lower_find_matching_refs, &decl, T_REF);
   return decl != NULL;
}

static int lower_count_target_parts(tree_t target, int depth)
{
   if (tree_kind(target) == T_AGGREGATE) {
      int count = 0;
      const int nassocs = tree_assocs(target);
      for (int i = 0; i < nassocs; i++) {
         tree_t value = tree_value(tree_assoc(target, i));
         count += lower_count_target_parts(value, depth + 1);
      }

      return count + (depth > 0 ? 2 : 1);
   }
   else
      return depth == 0 ? 2 : 1;
}

static void lower_fill_target_parts(tree_t target, part_kind_t kind,
                                    target_part_t **ptr)
{
   if (tree_kind(target) == T_AGGREGATE) {
      const bool is_record = type_is_record(tree_type(target));
      part_kind_t newkind = is_record ? PART_FIELD : PART_ELEM;

      if (kind != PART_ALL) {
         (*ptr)->reg  = VCODE_INVALID_REG;
         (*ptr)->type = NULL;
         (*ptr)->kind = kind == PART_FIELD ? PART_PUSH_FIELD : PART_PUSH_ELEM;
         ++(*ptr);
      }

      const int nassocs = tree_assocs(target);
      for (int i = 0; i < nassocs; i++) {
         tree_t value = tree_value(tree_assoc(target, i));
         lower_fill_target_parts(value, newkind, ptr);
      }

      (*ptr)->reg  = VCODE_INVALID_REG;
      (*ptr)->type = NULL;
      (*ptr)->kind = PART_POP;
      ++(*ptr);
   }
   else {
      (*ptr)->reg  = lower_expr(target, EXPR_LVALUE);
      (*ptr)->type = tree_type(target);
      (*ptr)->kind = kind;
      ++(*ptr);

      if (kind == PART_ALL) {
         (*ptr)->reg  = VCODE_INVALID_REG;
         (*ptr)->type = NULL;
         (*ptr)->kind = PART_POP;
         ++(*ptr);
      }
   }
}

static void lower_var_assign_target(target_part_t **ptr, tree_t where,
                                    vcode_reg_t rhs, type_t rhs_type)
{
   int fieldno = 0;
   for (const target_part_t *p = (*ptr)++; p->kind != PART_POP; p = (*ptr)++) {
      vcode_reg_t src_reg = rhs;
      type_t src_type = rhs_type;
      if (p->kind == PART_FIELD || p->kind == PART_PUSH_FIELD) {
         assert(vcode_reg_kind(rhs) == VCODE_TYPE_POINTER);
         src_reg = emit_record_ref(rhs, fieldno);
         src_type = tree_type(type_field(src_type, fieldno));
         fieldno++;
      }

      if (p->kind == PART_PUSH_FIELD || p->kind == PART_PUSH_ELEM) {
         lower_var_assign_target(ptr, where, src_reg, src_type);
         continue;
      }
      else if (p->reg == VCODE_INVALID_REG)
         continue;

      if (type_is_array(p->type))
         lower_check_array_sizes(where, p->type, src_type, p->reg, src_reg);

      if (p->kind == PART_ELEM)
         src_reg = lower_array_data(src_reg);

      if (type_is_scalar(p->type))
         lower_check_scalar_bounds(lower_reify(src_reg), p->type, where, NULL);

      if (lower_have_signal(src_reg))
         src_reg = emit_resolved(lower_array_data(rhs));

      if (type_is_array(p->type)) {
         vcode_reg_t data_reg = lower_array_data(src_reg);
         vcode_reg_t count_reg = lower_array_total_len(p->type, p->reg);

         emit_copy(p->reg, data_reg, count_reg);
      }
      else if (type_is_record(p->type))
         emit_copy(p->reg, src_reg, VCODE_INVALID_REG);
      else
         emit_store_indirect(lower_reify(src_reg), p->reg);

      if (p->kind == PART_ELEM) {
         assert(vcode_reg_kind(src_reg) == VCODE_TYPE_POINTER);
         rhs = emit_add(src_reg, emit_const(vtype_offset(), 1));
      }
   }
}

static void lower_var_assign(tree_t stmt)
{
   tree_t value  = tree_value(stmt);
   tree_t target = tree_target(stmt);
   type_t type   = tree_type(target);

   const bool is_var_decl =
      tree_kind(target) == T_REF && tree_kind(tree_ref(target)) == T_VAR_DECL;
   const bool is_scalar = type_is_scalar(type);
   const bool is_access = type_is_access(type);

   const int saved_mark = emit_temp_stack_mark();
   uint32_t hint = VCODE_INVALID_HINT;

   if (is_scalar || is_access) {
      vcode_reg_t value_reg = lower_expr(value, EXPR_RVALUE);
      vcode_reg_t loaded_value = lower_reify(value_reg);
      vcode_var_t var = VCODE_INVALID_VAR;
      int hops = 0;
      if (is_scalar)
         lower_check_scalar_bounds(loaded_value, type, stmt, NULL);
      else
         loaded_value = lower_incomplete_access(loaded_value,
                                                type_access(type));

      if (is_var_decl
          && (var = lower_get_var(tree_ref(target), &hops)) != VCODE_INVALID_VAR
          && hops == 0)
         emit_store(loaded_value, var);
      else
         emit_store_indirect(loaded_value, lower_expr(target, EXPR_LVALUE));
   }
   else if (tree_kind(target) == T_AGGREGATE) {
      const int nparts = lower_count_target_parts(target, 0);
      target_part_t *parts LOCAL = xmalloc_array(nparts, sizeof(target_part_t));

      target_part_t *ptr = parts;
      lower_fill_target_parts(target, PART_ALL, &ptr);
      assert(ptr == parts + nparts);

      vcode_reg_t rhs = lower_expr(value, EXPR_RVALUE);

      ptr = parts;
      lower_var_assign_target(&ptr, value, rhs, tree_type(value));
      assert(ptr == parts + nparts);
   }
   else if (type_is_array(type)) {
      vcode_reg_t target_reg  = lower_expr(target, EXPR_LVALUE);
      vcode_reg_t count_reg   = lower_array_total_len(type, target_reg);
      vcode_reg_t target_data = lower_array_data(target_reg);

      if (lower_assign_can_use_storage_hint(stmt))
         hint = emit_storage_hint(target_data, count_reg);

      vcode_reg_t value_reg = lower_expr(value, EXPR_RVALUE);
      vcode_reg_t src_data = lower_array_data(value_reg);
      lower_check_array_sizes(stmt, type, tree_type(value),
                              target_reg, value_reg);

      if (lower_have_signal(src_data))
         src_data = emit_resolved(src_data);

      emit_copy(target_data, src_data, count_reg);
   }
   else {
      vcode_reg_t value_reg  = lower_expr(value, EXPR_RVALUE);
      vcode_reg_t target_reg = lower_expr(target, EXPR_LVALUE);

      if (lower_assign_can_use_storage_hint(stmt))
         hint = emit_storage_hint(target_reg, VCODE_INVALID_REG);

      emit_copy(target_reg, value_reg, VCODE_INVALID_REG);
   }

   if (hint != VCODE_INVALID_HINT)
      vcode_clear_storage_hint(hint);

   emit_temp_stack_restore(saved_mark);
}

static void lower_signal_assign_target(target_part_t **ptr, tree_t where,
                                       vcode_reg_t rhs, type_t rhs_type,
                                       vcode_reg_t reject, vcode_reg_t after)
{
   int fieldno = 0;
   for (const target_part_t *p = (*ptr)++; p->kind != PART_POP; p = (*ptr)++) {
      vcode_reg_t src_reg = rhs;
      type_t src_type = rhs_type;
      if (p->kind == PART_FIELD || p->kind == PART_PUSH_FIELD) {
         assert(vcode_reg_kind(rhs) == VCODE_TYPE_POINTER);
         src_reg = emit_record_ref(rhs, fieldno);
         src_type = tree_type(type_field(src_type, fieldno));
         fieldno++;
      }

      if (p->kind == PART_PUSH_FIELD || p->kind == PART_PUSH_ELEM) {
         lower_signal_assign_target(ptr, where, src_reg, src_type,
                                    reject, after);
         continue;
      }
      else if (p->reg == VCODE_INVALID_REG)
         continue;

      if (type_is_array(p->type))
         lower_check_array_sizes(where, p->type, src_type, p->reg, src_reg);

      if (p->kind == PART_ELEM)
         src_reg = lower_array_data(src_reg);

      if (type_is_scalar(p->type))
         lower_check_scalar_bounds(lower_reify(src_reg), p->type, where, NULL);

      if (lower_have_signal(src_reg))
         src_reg = emit_resolved(lower_array_data(rhs));

      vcode_reg_t nets_raw = lower_array_data(p->reg);

      if (type_is_array(p->type)) {
         vcode_reg_t data_reg = lower_array_data(src_reg);
         vcode_reg_t count_reg = lower_scalar_sub_elements(p->type, p->reg);

         emit_sched_waveform(nets_raw, count_reg, data_reg, reject, after);
      }
      else if (type_is_record(p->type)) {
         const int width = type_width(p->type);
         emit_sched_waveform(nets_raw, emit_const(vtype_offset(), width),
                             src_reg, reject, after);
      }
      else {
         emit_sched_waveform(nets_raw, emit_const(vtype_offset(), 1),
                             src_reg, reject, after);
      }

      if (p->kind == PART_ELEM) {
         assert(vcode_reg_kind(src_reg) == VCODE_TYPE_POINTER);
         rhs = emit_add(src_reg, emit_const(vtype_offset(), 1));
      }
   }
}

static void lower_disconnect_target(target_part_t **ptr, vcode_reg_t reject,
                                    vcode_reg_t after)
{
   for (const target_part_t *p = (*ptr)++; p->kind != PART_POP; p = (*ptr)++) {
      if (p->kind == PART_PUSH_FIELD || p->kind == PART_PUSH_ELEM) {
         lower_disconnect_target(ptr, reject, after);
         continue;
      }
      else if (p->reg == VCODE_INVALID_REG)
         continue;

      vcode_reg_t nets_raw = lower_array_data(p->reg);

      if (type_is_array(p->type)) {
         vcode_reg_t count_reg = lower_scalar_sub_elements(p->type, p->reg);
         emit_disconnect(nets_raw, count_reg, reject, after);
      }
      else if (type_is_record(p->type)) {
         const int width = type_width(p->type);
         emit_disconnect(nets_raw, emit_const(vtype_offset(), width),
                         reject, after);
      }
      else {
         emit_disconnect(nets_raw, emit_const(vtype_offset(), 1),
                         reject, after);
      }
   }
}

static void lower_signal_assign(tree_t stmt)
{
   const int saved_mark = emit_temp_stack_mark();

   vcode_reg_t reject;
   if (tree_has_reject(stmt))
      reject = lower_reify_expr(tree_reject(stmt));
   else
      reject = emit_const(vtype_int(INT64_MIN, INT64_MAX), 0);

   tree_t target = tree_target(stmt);

   const int nparts = lower_count_target_parts(target, 0);
   target_part_t *parts LOCAL = xmalloc_array(nparts, sizeof(target_part_t));

   target_part_t *ptr = parts;
   lower_fill_target_parts(target, PART_ALL, &ptr);
   assert(ptr == parts + nparts);

   const int nwaveforms = tree_waveforms(stmt);
   for (int i = 0; i < nwaveforms; i++) {
      tree_t w = tree_waveform(stmt, i);

      vcode_reg_t after;
      if (tree_has_delay(w))
         after = lower_expr(tree_delay(w), EXPR_RVALUE);
      else
         after = emit_const(vtype_int(INT64_MIN, INT64_MAX), 0);

      target_part_t *ptr = parts;
      if (tree_has_value(w)) {
         tree_t wvalue = tree_value(w);
         type_t wtype = tree_type(wvalue);
         vcode_reg_t rhs = lower_expr(wvalue, EXPR_RVALUE);
         lower_signal_assign_target(&ptr, wvalue, rhs, wtype, reject, after);
      }
      else
         lower_disconnect_target(&ptr, reject, after);
      assert(ptr == parts + nparts);

      // All but the first waveform have zero reject time
      if (nwaveforms > 1 && tree_has_reject(stmt))
         reject = emit_const(vtype_int(INT64_MIN, INT64_MAX), 0);
   }

   emit_temp_stack_restore(saved_mark);
}

static vcode_reg_t lower_test_expr(tree_t value)
{
   const int saved_mark = emit_temp_stack_mark();
   vcode_reg_t test = lower_reify_expr(value);
   emit_temp_stack_restore(saved_mark);
   lower_cond_coverage(value, test);
   return test;
}

static void lower_if(tree_t stmt, loop_stack_t *loops)
{
   vcode_reg_t test = lower_test_expr(tree_value(stmt));

   const int nelses = tree_else_stmts(stmt);
   const int nstmts = tree_stmts(stmt);

   int64_t cval;
   if (vcode_reg_const(test, &cval)) {
      emit_comment("Condition of if statement line %d is always %s",
                   tree_loc(stmt)->first_line, cval ? "true" : "false");
      if (cval) {
         for (int i = 0; i < nstmts; i++)
            lower_stmt(tree_stmt(stmt, i), loops);
      }
      else {
         for (int i = 0; i < nelses; i++)
            lower_stmt(tree_else_stmt(stmt, i), loops);
      }

      return;
   }

   vcode_block_t btrue = emit_block();
   vcode_block_t bfalse = nelses > 0 ? emit_block() : VCODE_INVALID_BLOCK;
   vcode_block_t bmerge = nelses > 0 ? VCODE_INVALID_BLOCK : emit_block();

   emit_cond(test, btrue, nelses > 0 ? bfalse : bmerge);

   vcode_select_block(btrue);

   for (int i = 0; i < nstmts; i++)
      lower_stmt(tree_stmt(stmt, i), loops);

   if (!vcode_block_finished()) {
      if (bmerge == VCODE_INVALID_BLOCK)
         bmerge = emit_block();
      emit_jump(bmerge);
   }

   if (nelses > 0) {
      vcode_select_block(bfalse);

      for (int i = 0; i < nelses; i++)
         lower_stmt(tree_else_stmt(stmt, i), loops);

      if (!vcode_block_finished()) {
         if (bmerge == VCODE_INVALID_BLOCK)
            bmerge = emit_block();
         emit_jump(bmerge);
      }
   }

   if (bmerge != VCODE_INVALID_BLOCK)
      vcode_select_block(bmerge);
}

static void lower_cleanup_protected(void)
{
   if (!(top_scope->flags & SCOPE_HAS_PROTECTED))
      return;
   else if (!is_subprogram(top_scope->container))
      return;

   const int ndecls = tree_decls(top_scope->container);
   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(top_scope->container, i);
      if (!type_is_protected(tree_type(d)))
         continue;

      vcode_reg_t obj_reg = lower_reify(lower_var_ref(d, EXPR_RVALUE));
      emit_protected_free(obj_reg);
   }
}

static void lower_return(tree_t stmt)
{
   if (tree_has_value(stmt)) {
      tree_t value = tree_value(stmt);

      vtype_kind_t result_kind = vtype_kind(vcode_unit_result());

      type_t type = tree_type(value);
      if (type_is_scalar(type)) {
         vcode_reg_t result = lower_reify_expr(value);
         lower_check_scalar_bounds(result, type, value, NULL);
         emit_return(result);
      }
      else if (result_kind == VCODE_TYPE_UARRAY) {
         vcode_reg_t array = lower_expr(value, EXPR_RVALUE);

         if (vtype_kind(vcode_reg_type(array)) == VCODE_TYPE_UARRAY)
            emit_return(array);
         else
            emit_return(lower_wrap(type, lower_array_data(array)));
      }
      else if (result_kind == VCODE_TYPE_POINTER)
         emit_return(lower_array_data(lower_expr(value, EXPR_RVALUE)));
      else
         emit_return(lower_expr(value, EXPR_RVALUE));
   }
   else
      emit_return(VCODE_INVALID_REG);
}

static void lower_pcall(tree_t pcall)
{
   tree_t decl = tree_ref(pcall);

   const int saved_mark = emit_temp_stack_mark();

   const subprogram_kind_t kind = tree_subkind(decl);
   if (is_builtin(kind)) {
      lower_builtin(pcall, kind);
      emit_temp_stack_restore(saved_mark);
      return;
   }

   const bool never_waits = !!(tree_flags(decl) & TREE_F_NEVER_WAITS);
   const bool use_fcall =
      never_waits || vcode_unit_kind() == VCODE_UNIT_FUNCTION;

   const int nparams = tree_params(pcall);
   SCOPED_A(vcode_reg_t) args = AINIT;

   const vcode_cc_t cc = lower_cc_for_call(pcall);
   ident_t name = tree_ident2(decl);

   if (tree_kind(pcall) == T_PROT_PCALL && tree_has_name(pcall))
      APUSH(args, lower_reify(lower_expr(tree_name(pcall), EXPR_RVALUE)));
   else if (cc != VCODE_CC_FOREIGN)
      APUSH(args, lower_context_for_call(name));

   for (int i = 0; i < nparams; i++) {
      vcode_reg_t arg = lower_subprogram_arg(pcall, i);
      if (!use_fcall)
         vcode_heap_allocate(arg);
      APUSH(args, arg);
   }

   if (use_fcall) {
      emit_fcall(name, VCODE_INVALID_TYPE, VCODE_INVALID_TYPE,
                 cc, args.items, args.count);
      emit_temp_stack_restore(saved_mark);
   }
   else {
      vcode_block_t resume_bb = emit_block();

      // Save the temp stack mark in a variable so it is preserved
      // across suspend/resume
      vcode_var_t tmp_mark_var;
      ident_t tmp_mark_i = ident_new("tmp_mark");
      if ((tmp_mark_var = vcode_find_var(tmp_mark_i)) == VCODE_INVALID_VAR)
         tmp_mark_var = emit_var(vtype_offset(), vtype_offset(), tmp_mark_i, 0);
      emit_store(saved_mark, tmp_mark_var);

      emit_pcall(name, args.items, args.count, resume_bb);
      vcode_select_block(resume_bb);
      emit_resume(name);

      emit_temp_stack_restore(emit_load(tmp_mark_var));
   }
}

static void lower_for(tree_t stmt, loop_stack_t *loops)
{
   tree_t r = tree_range(stmt, 0);
   vcode_reg_t left_reg  = lower_range_left(r);
   vcode_reg_t right_reg = lower_range_right(r);
   vcode_reg_t dir_reg   = lower_range_dir(r);
   vcode_reg_t null_reg  = emit_range_null(left_reg, right_reg, dir_reg);

   vcode_block_t exit_bb = VCODE_INVALID_BLOCK;

   int64_t null_const;
   if (vcode_reg_const(null_reg, &null_const) && null_const)
      return;   // Loop range is always null
   else {
      vcode_block_t init_bb = emit_block();
      exit_bb = emit_block();
      emit_cond(null_reg, exit_bb, init_bb);
      vcode_select_block(init_bb);
   }

   tree_t idecl = tree_decl(stmt, 0);

   vcode_type_t vtype  = lower_type(tree_type(idecl));
   vcode_type_t bounds = vtype;

   int64_t lconst, rconst, dconst;
   const bool l_is_const = vcode_reg_const(left_reg, &lconst);
   const bool r_is_const = vcode_reg_const(right_reg, &rconst);
   if (l_is_const && r_is_const)
      bounds = vtype_int(MIN(lconst, rconst), MAX(lconst, rconst));
   else if ((l_is_const || r_is_const) && vcode_reg_const(dir_reg, &dconst)) {
      if (dconst == RANGE_TO)
         bounds = vtype_int(l_is_const ? lconst : vtype_low(vtype),
                            r_is_const ? rconst : vtype_high(vtype));
      else
         bounds = vtype_int(r_is_const ? rconst : vtype_low(vtype),
                            l_is_const ? lconst : vtype_high(vtype));
   }

   ident_t ident = ident_prefix(tree_ident(idecl), tree_ident(stmt), '.');
   vcode_var_t ivar = emit_var(vtype, bounds, ident, 0);
   lower_put_vcode_obj(idecl, ivar, top_scope);

   emit_store(left_reg, ivar);

   vcode_block_t body_bb = emit_block();
   emit_jump(body_bb);
   vcode_select_block(body_bb);

   if (exit_bb == VCODE_INVALID_BLOCK)
      exit_bb = emit_block();

   loop_stack_t this = {
      .up      = loops,
      .name    = tree_ident(stmt),
      .test_bb = VCODE_INVALID_BLOCK,
      .exit_bb = exit_bb
   };

   const int nstmts = tree_stmts(stmt);
   for (int i = 0; i < nstmts; i++)
      lower_stmt(tree_stmt(stmt, i), &this);

   if (this.test_bb != VCODE_INVALID_BLOCK) {
      // Loop body contained a "next" statement
      if (!vcode_block_finished())
         emit_jump(this.test_bb);
      vcode_select_block(this.test_bb);
   }

   vcode_reg_t dirn_reg  = lower_range_dir(r);
   vcode_reg_t step_down = emit_const(vtype, -1);
   vcode_reg_t step_up   = emit_const(vtype, 1);
   vcode_reg_t step_reg  = emit_select(dirn_reg, step_down, step_up);
   vcode_reg_t ireg      = emit_load(ivar);
   vcode_reg_t next_reg  = emit_add(ireg, step_reg);
   emit_store(next_reg, ivar);

   vcode_reg_t final_reg = lower_range_right(r);
   vcode_reg_t done_reg = emit_cmp(VCODE_CMP_EQ, ireg, final_reg);
   emit_cond(done_reg, exit_bb, body_bb);

   vcode_select_block(exit_bb);
}

static void lower_while(tree_t stmt, loop_stack_t *loops)
{
   vcode_block_t test_bb, body_bb, exit_bb;
   if (tree_has_value(stmt)) {
      test_bb = emit_block();
      body_bb = emit_block();
      exit_bb = emit_block();

      emit_jump(test_bb);

      vcode_select_block(test_bb);

      vcode_reg_t test = lower_test_expr(tree_value(stmt));
      emit_cond(test, body_bb, exit_bb);
   }
   else {
      test_bb = body_bb =
         vcode_block_empty() ? vcode_active_block() : emit_block();
      exit_bb = emit_block();

      emit_jump(body_bb);
   }

   vcode_select_block(body_bb);

   loop_stack_t this = {
      .up      = loops,
      .name    = tree_ident(stmt),
      .test_bb = test_bb,
      .exit_bb = exit_bb
   };

   const int nstmts = tree_stmts(stmt);
   for (int i = 0; i < nstmts; i++)
      lower_stmt(tree_stmt(stmt, i), &this);

   if (!vcode_block_finished())
      emit_jump(test_bb);

   vcode_select_block(exit_bb);
}

static void lower_block(tree_t block, loop_stack_t *loops)
{
   const int nstmts = tree_stmts(block);
   for (int i = 0; i < nstmts; i++)
      lower_stmt(tree_stmt(block, i), loops);
}

static void lower_loop_control(tree_t stmt, loop_stack_t *loops)
{
   vcode_block_t false_bb = emit_block();

   if (tree_has_value(stmt)) {
      vcode_block_t true_bb = emit_block();

      vcode_reg_t result = lower_test_expr(tree_value(stmt));
      emit_cond(result, true_bb, false_bb);

      vcode_select_block(true_bb);
   }

   ident_t label = tree_ident2(stmt);
   loop_stack_t *it;
   for (it = loops; it != NULL && it->name != label; it = it->up)
      ;
   assert(it != NULL);

   if (tree_kind(stmt) == T_EXIT)
      emit_jump(it->exit_bb);
   else {
      if (it->test_bb == VCODE_INVALID_BLOCK)
         it->test_bb = emit_block();
      emit_jump(it->test_bb);
   }

   vcode_select_block(false_bb);
}

static void lower_case_scalar(tree_t stmt, loop_stack_t *loops)
{
   const int nassocs = tree_assocs(stmt);

   vcode_block_t def_bb = VCODE_INVALID_BLOCK;
   vcode_block_t exit_bb = emit_block();
   vcode_block_t hit_bb = VCODE_INVALID_BLOCK;

   vcode_reg_t value_reg = lower_reify_expr(tree_value(stmt));

   tree_t last = NULL;

   for (int i = 0; i < nassocs; i++) {
      tree_t a = tree_assoc(stmt, i);

      if (tree_subkind(a) == A_RANGE) {
         // Pre-filter range choices in case the number of elements is large
         tree_t r = tree_range(a, 0);
         vcode_reg_t left_reg = lower_range_left(r);
         vcode_reg_t right_reg = lower_range_right(r);

         const range_kind_t dir = tree_subkind(r);
         vcode_reg_t low_reg = dir == RANGE_TO ? left_reg : right_reg;
         vcode_reg_t high_reg = dir == RANGE_TO ? right_reg : left_reg;

         vcode_reg_t lcmp_reg = emit_cmp(VCODE_CMP_GEQ, value_reg, low_reg);
         vcode_reg_t hcmp_reg = emit_cmp(VCODE_CMP_LEQ, value_reg, high_reg);
         vcode_reg_t hit_reg = emit_and(lcmp_reg, hcmp_reg);

         vcode_block_t skip_bb = emit_block();

         tree_t block = tree_value(a);
         if (block != last) hit_bb = emit_block();

         emit_cond(hit_reg, hit_bb, skip_bb);

         if (stmt != last) {
            vcode_select_block(hit_bb);
            lower_stmt(block, loops);
            if (!vcode_block_finished())
               emit_jump(exit_bb);
         }

         last = block;
         vcode_select_block(skip_bb);
      }
   }

   vcode_block_t start_bb = vcode_active_block();

   vcode_reg_t *cases LOCAL = xcalloc_array(nassocs, sizeof(vcode_reg_t));
   vcode_block_t *blocks LOCAL = xcalloc_array(nassocs, sizeof(vcode_block_t));

   last = NULL;
   hit_bb = VCODE_INVALID_BLOCK;

   int cptr = 0;
   for (int i = 0; i < nassocs; i++) {
      tree_t a = tree_assoc(stmt, i);
      const assoc_kind_t kind = tree_subkind(a);

      if (kind == A_RANGE)
         continue;    // Handled separately above

      tree_t block = tree_value(a);
      if (block != last) hit_bb = emit_block();

      if (kind == A_OTHERS)
         def_bb = hit_bb;
      else {
         vcode_select_block(start_bb);
         cases[cptr]  = lower_reify_expr(tree_name(a));
         blocks[cptr] = hit_bb;
         cptr++;
      }

      if (block != last) {
         vcode_select_block(hit_bb);
         lower_stmt(block, loops);
         if (!vcode_block_finished())
            emit_jump(exit_bb);
      }

      last = block;
   }

   if (def_bb == VCODE_INVALID_BLOCK)
      def_bb = exit_bb;

   vcode_select_block(start_bb);
   emit_case(value_reg, def_bb, cases, blocks, cptr);

   vcode_select_block(exit_bb);
}

static void lower_case_array(tree_t stmt, loop_stack_t *loops)
{
   vcode_block_t def_bb   = VCODE_INVALID_BLOCK;
   vcode_block_t exit_bb  = emit_block();
   vcode_block_t hit_bb   = VCODE_INVALID_BLOCK;
   vcode_block_t start_bb = vcode_active_block();

   tree_t value = tree_value(stmt);
   type_t type = tree_type(value);
   vcode_reg_t val_reg = lower_expr(tree_value(stmt), EXPR_RVALUE);
   vcode_reg_t data_ptr = lower_array_data(val_reg);

   vcode_type_t vint64 = vtype_int(INT64_MIN, INT64_MAX);
   vcode_type_t voffset = vtype_offset();

   int64_t length;
   if (!folded_length(range_of(type, 0), &length))
      fatal_at(tree_loc(value), "array length is not known at compile time");

   type_t base = type_base_recur(type_elem(type));
   assert(type_kind(base) == T_ENUM);

   const int nbits = ilog2(type_enum_literals(base));
   const bool exact_map = length * nbits <= 64;

   // Limit the number of cases branches we generate so it can be
   // efficiently implemented with a jump table
   static const int max_cases = 256;

   if (!exact_map) {
      // Hash function may have collisions so need to emit calls to
      // comparison function
      if (vcode_reg_kind(val_reg) != VCODE_TYPE_UARRAY)
         val_reg = lower_wrap(type, val_reg);
   }

   vcode_type_t enc_type = VCODE_INVALID_TYPE;
   vcode_reg_t enc_reg = VCODE_INVALID_REG;
   if (exact_map && length <= 4) {
      // Unroll the encoding calculation
      enc_type = voffset;
      enc_reg = emit_const(enc_type, 0);
      for (int64_t i = 0; i < length; i++) {
         vcode_reg_t ptr_reg  = emit_add(data_ptr, emit_const(voffset, i));
         vcode_reg_t byte_reg = emit_load_indirect(ptr_reg);
         enc_reg = emit_mul(enc_reg, emit_const(enc_type, 1 << nbits));
         enc_reg = emit_add(enc_reg, emit_cast(enc_type, enc_type, byte_reg));
      }
   }
   else {
      enc_type = vint64;
      vcode_var_t enc_var = emit_var(enc_type, enc_type, ident_uniq("enc"), 0);
      emit_store(emit_const(enc_type, 0), enc_var);

      vcode_var_t i_var = emit_var(voffset, voffset, ident_uniq("i"), 0);
      emit_store(emit_const(voffset, 0), i_var);

      vcode_block_t body_bb = emit_block();
      vcode_block_t exit_bb = start_bb = emit_block();

      emit_jump(body_bb);

      vcode_select_block(body_bb);

      vcode_reg_t i_reg = emit_load(i_var);
      vcode_reg_t ptr_reg  = emit_add(data_ptr, i_reg);
      vcode_reg_t byte_reg = emit_load_indirect(ptr_reg);
      vcode_reg_t tmp_reg = emit_load(enc_var);
      if (exact_map)
         tmp_reg = emit_mul(tmp_reg, emit_const(enc_type, 1 << nbits));
      else
         tmp_reg = emit_mul(tmp_reg, emit_const(enc_type, 0x27d4eb2d));
      tmp_reg = emit_add(tmp_reg, emit_cast(enc_type, enc_type, byte_reg));
      emit_store(tmp_reg, enc_var);

      vcode_reg_t i_next = emit_add(i_reg, emit_const(voffset, 1));
      emit_store(i_next, i_var);

      vcode_reg_t done_reg = emit_cmp(VCODE_CMP_EQ, i_next,
                                      emit_const(voffset, length));
      emit_cond(done_reg, exit_bb, body_bb);

      vcode_select_block(exit_bb);

      enc_reg = emit_load(enc_var);

      if (!exact_map)
         enc_reg = emit_rem(enc_reg, emit_const(enc_type, max_cases));
   }

   const int nassocs = tree_assocs(stmt);
   vcode_reg_t *cases LOCAL = xcalloc_array(nassocs, sizeof(vcode_reg_t));
   vcode_block_t *blocks LOCAL = xcalloc_array(nassocs, sizeof(vcode_block_t));
   int64_t *encoding LOCAL = xcalloc_array(nassocs, sizeof(int64_t));

   tree_t last = NULL;
   ident_t cmp_func = NULL;
   vcode_type_t vbool = vtype_bool();
   vcode_block_t fallthrough_bb = VCODE_INVALID_BLOCK;

   if (!exact_map) {
      fallthrough_bb = emit_block();
      cmp_func = lower_predef_func_name(tree_type(value), "=");
   }

   int ndups = 0;
   int cptr = 0;
   for (int i = 0; i < nassocs; i++) {
      tree_t a = tree_assoc(stmt, i);
      const assoc_kind_t kind = tree_subkind(a);
      assert(kind != A_RANGE);

      tree_t block = tree_value(a);
      if (block != last) hit_bb = emit_block();

      if (kind == A_OTHERS)
         def_bb = hit_bb;
      else {
         tree_t name = tree_name(a);
         int64_t enc = encode_case_choice(name, length, exact_map ? nbits : 0);
         if (!exact_map) enc %= max_cases;

         vcode_block_t entry_bb = hit_bb;
         bool have_dup = false;
         if (!exact_map) {
            // There may be collisions in the hash function
            vcode_block_t chain_bb = fallthrough_bb;
            for (int j = 0; j < cptr; j++) {
               if (encoding[j] == enc) {
                  ndups++;
                  chain_bb  = blocks[j];
                  blocks[j] = hit_bb;
                  have_dup  = true;
                  break;
               }
            }

            vcode_select_block(hit_bb);
            hit_bb = emit_block();

            vcode_reg_t name_reg = lower_expr(name, EXPR_RVALUE);
            if (vcode_reg_kind(name_reg) != VCODE_TYPE_UARRAY)
               name_reg = lower_wrap(type, name_reg);

            vcode_reg_t context_reg = lower_context_for_call(cmp_func);
            vcode_reg_t args[] = { context_reg, name_reg, val_reg };
            vcode_reg_t eq_reg = emit_fcall(cmp_func, vbool, vbool,
                                            VCODE_CC_PREDEF, args, 3);
            emit_cond(eq_reg, hit_bb, chain_bb);
         }

         if (!have_dup) {
            vcode_select_block(start_bb);
            cases[cptr]    = emit_const(enc_type, enc);
            blocks[cptr]   = entry_bb;
            encoding[cptr] = enc;
            cptr++;
         }
      }

      if (block != last) {
         vcode_select_block(hit_bb);
         lower_stmt(block, loops);
         if (!vcode_block_finished())
            emit_jump(exit_bb);
      }

      last = block;
   }

   if (def_bb == VCODE_INVALID_BLOCK)
      def_bb = exit_bb;

   if (fallthrough_bb != VCODE_INVALID_BLOCK) {
      vcode_select_block(fallthrough_bb);
      emit_jump(def_bb);
   }

   vcode_select_block(start_bb);
   emit_case(enc_reg, def_bb, cases, blocks, cptr);

   vcode_select_block(exit_bb);
}

static void lower_case(tree_t stmt, loop_stack_t *loops)
{
   if (type_is_scalar(tree_type(tree_value(stmt))))
      lower_case_scalar(stmt, loops);
   else
      lower_case_array(stmt, loops);
}

static void lower_stmt(tree_t stmt, loop_stack_t *loops)
{
   PUSH_DEBUG_INFO(stmt);

   if (vcode_block_finished())
      return;   // Unreachable

   int32_t stmt_tag;
   if (cover_is_tagged(cover_tags, stmt, &stmt_tag, NULL))
      emit_cover_stmt(stmt_tag);

   emit_debug_info(tree_loc(stmt));

   switch (tree_kind(stmt)) {
   case T_ASSERT:
      lower_assert(stmt);
      break;
   case T_WAIT:
      lower_wait(stmt);
      break;
   case T_VAR_ASSIGN:
      lower_var_assign(stmt);
      break;
   case T_SIGNAL_ASSIGN:
      lower_signal_assign(stmt);
      break;
   case T_IF:
      lower_if(stmt, loops);
      break;
   case T_RETURN:
      lower_return(stmt);
      break;
   case T_PCALL:
   case T_PROT_PCALL:
      lower_pcall(stmt);
      break;
   case T_WHILE:
      lower_while(stmt, loops);
      break;
   case T_FOR:
      lower_for(stmt, loops);
      break;
   case T_BLOCK:
      lower_block(stmt, loops);
      break;
   case T_EXIT:
   case T_NEXT:
      lower_loop_control(stmt, loops);
      break;
   case T_CASE:
      lower_case(stmt, loops);
      break;
   default:
      fatal_at(tree_loc(stmt), "cannot lower statement kind %s",
               tree_kind_str(tree_kind(stmt)));
   }
}

static void lower_check_indexes(type_t type, vcode_reg_t array, tree_t hint)
{
   PUSH_DEBUG_INFO(hint);

   const int ndims = dimension_of(type);
   for (int i = 0; i < ndims; i++) {
      type_t index = index_type_of(type, i);

      vcode_type_t vbounds = lower_bounds(index);

      vcode_reg_t left_reg  = lower_array_left(type, i, array);
      vcode_reg_t right_reg = lower_array_right(type, i, array);

      if (type_is_enum(index))
         emit_index_check(left_reg, right_reg, vbounds, BOUNDS_INDEX_TO);
      else {
         tree_t rindex = range_of(index, 0);
         bounds_kind_t bkind  = tree_subkind(rindex) == RANGE_TO
            ? BOUNDS_INDEX_TO : BOUNDS_INDEX_DOWNTO;

         vcode_reg_t rlow_reg, rhigh_reg;
         if (lower_const_bounds(type)) {
            tree_t r = range_of(type, i);
            if (tree_subkind(r) == RANGE_TO) {
               rlow_reg  = left_reg;
               rhigh_reg = right_reg;
            }
            else {
               rlow_reg  = right_reg;
               rhigh_reg = left_reg;
            }
         }
         else {
            vcode_reg_t dir_reg = lower_array_dir(type, i, array);
            rlow_reg  = emit_select(dir_reg, right_reg, left_reg);
            rhigh_reg = emit_select(dir_reg, left_reg, right_reg);
         }

         tree_t rindex_left = tree_left(rindex);
         tree_t rindex_right = tree_right(rindex);

         if (lower_is_const(rindex_left) && lower_is_const(rindex_right)) {
            emit_index_check(rlow_reg, rhigh_reg, vbounds, bkind);
         }
         else {
            vcode_reg_t bleft  = lower_reify_expr(rindex_left);
            vcode_reg_t bright = lower_reify_expr(rindex_right);

            vcode_reg_t bmin = bkind == BOUNDS_INDEX_TO ? bleft : bright;
            vcode_reg_t bmax = bkind == BOUNDS_INDEX_TO ? bright : bleft;

            emit_dynamic_index_check(rlow_reg, rhigh_reg, bmin, bmax, bkind);
         }
      }
   }
}

static void lower_var_decl(tree_t decl)
{
   type_t type = tree_type(decl);
   vcode_type_t vtype = lower_type(type);
   vcode_type_t vbounds = lower_bounds(type);
   const bool is_global = !!(top_scope->flags & SCOPE_GLOBAL);
   const bool is_const = tree_kind(decl) == T_CONST_DECL;
   ident_t name = is_global ? tree_ident2(decl) : tree_ident(decl);

   bool skip_copy = false;
   if (is_const && !tree_has_value(decl)) {
      // Deferred constant in package
      return;
   }
   else if (is_const && type_is_array(type)
            && !lower_const_bounds(type)  // TODO: remove this restriction
            && lower_is_const(tree_value(decl))) {
      skip_copy = true;   // Will be allocated in constant data
   }

   vcode_var_flags_t flags = 0;
   if (is_const) flags |= VAR_CONST;
   if (is_global) flags |= VAR_GLOBAL;

   vcode_var_t var = emit_var(vtype, vbounds, name, flags);
   lower_put_vcode_obj(decl, var, top_scope);

   if (type_is_protected(type)) {
      vcode_reg_t context_reg = lower_context_for_call(type_ident(type));
      vcode_reg_t obj_reg = emit_protected_init(lower_type(type), context_reg);
      emit_store(obj_reg, var);
      top_scope->flags |= SCOPE_HAS_PROTECTED;
      return;
   }
   else if (!tree_has_value(decl))
      return;

   tree_t value = tree_value(decl);
   type_t value_type = tree_type(value);

   emit_debug_info(tree_loc(decl));

   vcode_reg_t dest_reg  = VCODE_INVALID_REG;
   vcode_reg_t count_reg = VCODE_INVALID_REG;
   uint32_t hint = VCODE_INVALID_HINT;

   const vunit_kind_t vunit_kind = vcode_unit_kind();
   const bool need_heap_alloc =
      vunit_kind == VCODE_UNIT_PROCEDURE
      || vunit_kind == VCODE_UNIT_PROCESS
      || vunit_kind == VCODE_UNIT_PACKAGE
      || vunit_kind == VCODE_UNIT_INSTANCE
      || vunit_kind == VCODE_UNIT_PROTECTED;

   if (type_is_record(type)) {
      dest_reg = emit_index(var, VCODE_INVALID_REG);
      hint = emit_storage_hint(dest_reg, VCODE_INVALID_REG);
   }
   else if (type_is_array(type) && !type_is_unconstrained(type)) {
      count_reg = lower_array_total_len(type, VCODE_INVALID_REG);

      if (!lower_const_bounds(type)) {
         type_t scalar_elem = lower_elem_recur(type);
         dest_reg = emit_alloca(lower_type(scalar_elem),
                                lower_bounds(scalar_elem),
                                count_reg);
         emit_store(lower_wrap(type, dest_reg), var);

         if (need_heap_alloc)
            vcode_heap_allocate(dest_reg);
      }
      else
         dest_reg = emit_index(var, VCODE_INVALID_REG);

      hint = emit_storage_hint(dest_reg, count_reg);
   }

   vcode_reg_t value_reg = lower_expr(value, EXPR_RVALUE);

   if (hint != VCODE_INVALID_HINT)
      vcode_clear_storage_hint(hint);

   if (type_is_array(type)) {
      vcode_reg_t data_reg = lower_array_data(value_reg);
      if (lower_have_signal(data_reg))
         data_reg = emit_resolved(data_reg);

      if (is_const && skip_copy) {
         if (type_is_unconstrained(type)) {
            vcode_reg_t wrapped_reg = lower_wrap(value_type, data_reg);
            emit_store(wrapped_reg, var);
         }
         else
            assert(false);   // TODO: this needs vtype adjusted above
      }
      else if (type_is_unconstrained(type)) {
         count_reg = lower_array_total_len(value_type, value_reg);

         type_t scalar_elem = lower_elem_recur(type);
         dest_reg = emit_alloca(lower_type(scalar_elem),
                                lower_bounds(scalar_elem),
                                count_reg);
         emit_copy(dest_reg, data_reg, count_reg);
         vcode_reg_t wrapped_reg =
            lower_wrap_with_new_bounds(value_type, value_reg, dest_reg);
         emit_store(wrapped_reg, var);

         if (need_heap_alloc)
            vcode_heap_allocate(dest_reg);
      }
      else {
         lower_check_indexes(type, value_reg, decl);
         lower_check_array_sizes(decl, type, value_type,
                                 VCODE_INVALID_REG, value_reg);
         emit_copy(dest_reg, data_reg, count_reg);
      }
   }
   else if (type_is_record(type)) {
      emit_copy(dest_reg, value_reg, VCODE_INVALID_REG);
   }
   else if (type_is_scalar(type)) {
      value_reg = lower_reify(value_reg);
      lower_check_scalar_bounds(value_reg, type, decl, NULL);
      emit_store(value_reg, var);
   }
   else if (type_is_access(type))
      emit_store(lower_incomplete_access(lower_reify(value_reg),
                                         type_access(type)), var);
   else
      emit_store(value_reg, var);
}

static vcode_reg_t lower_resolution_func(type_t type)
{
   tree_t rname = NULL;
   if (type_kind(type) == T_SUBTYPE) {
      if (type_has_resolution(type))
         rname = type_resolution(type);
      else if (type_is_array(type)) {
         // Special handling for subtype created when object is decalared
         type_t base = type_base(type);
         if (type_kind(base) == T_SUBTYPE && type_is_unconstrained(base)
             && type_has_resolution(base))
            rname = type_resolution(base);
      }
   }

   if (rname == NULL) {
      if (type_is_array(type))
         return lower_resolution_func(type_elem(type));
      else
         return VCODE_INVALID_REG;
   }

   while (tree_kind(rname) == T_AGGREGATE) {
      assert(type_is_array(type));
      assert(tree_assocs(rname) == 1);

      rname = tree_value(tree_assoc(rname, 0));
      type = type_elem(type);
   }

   tree_t rdecl = tree_ref(rname);
   ident_t rfunc = tree_ident2(rdecl);
   vcode_type_t vtype = lower_type(type);

   type_t uarray_param = type_param(tree_type(rdecl), 0);
   assert(type_kind(uarray_param) == T_ARRAY);
   tree_t r = range_of(type_index_constr(uarray_param, 0), 0);

   vcode_reg_t ileft_reg = emit_const(vtype_offset(), assume_int(tree_left(r)));

   vcode_reg_t nlits_reg;
   if (type_is_enum(type)) {
      // This resolution function can potentially be memoised
      if (type_kind(type) == T_SUBTYPE) {
         int64_t low, high;
         range_bounds(range_of(type, 0), &low, &high);
         nlits_reg = emit_const(vtype_offset(), high - low + 1);
      }
      else
         nlits_reg = emit_const(vtype_offset(), type_enum_literals(type));
   }
   else
      nlits_reg = emit_const(vtype_offset(), 0);

   const bool is_carray = vtype_kind(vtype) == VCODE_TYPE_CARRAY;
   vcode_type_t elem = is_carray ? vtype_elem(vtype) : vtype;
   vcode_type_t rtype = vtype_is_composite(vtype) ? vtype_pointer(elem) : vtype;
   vcode_type_t atype = vtype_uarray(1, elem, vtype_int(0, INT32_MAX));

   vcode_reg_t context_reg = lower_context_for_call(rfunc);
   vcode_reg_t closure_reg = emit_closure(rfunc, context_reg, atype, rtype);
   return emit_resolution_wrapper(rtype, closure_reg, ileft_reg, nlits_reg);
}

static void lower_sub_signals(type_t type, tree_t where, vcode_reg_t subsig,
                              vcode_reg_t init_reg, vcode_reg_t resolution)
{
   if (resolution == VCODE_INVALID_REG)
      resolution = lower_resolution_func(type);

   if (type_is_homogeneous(type)) {
      vcode_reg_t size_reg = emit_const(vtype_offset(), lower_byte_width(type));
      vcode_reg_t len_reg;
      if (type_is_array(type)) {
         len_reg = lower_array_total_len(type, init_reg);
         init_reg = lower_array_data(init_reg);
         // TODO: need array size check here
      }
      else {
         len_reg = emit_const(vtype_offset(), 1);
         init_reg = lower_reify(init_reg);
         lower_check_scalar_bounds(init_reg, type, where, where);
      }

      emit_init_signal(subsig, init_reg, len_reg, size_reg, resolution);
   }
   else if (type_is_array(type)) {
      // Array of non-homogeneous type (e.g. records). Need a loop to
      // initialise each sub-signal.

      const int ndims = dimension_of(type);
      vcode_reg_t len_reg = lower_array_len(type, 0, init_reg);
      for (int i = 1; i < ndims; i++)
         len_reg = emit_mul(lower_array_len(type, i, init_reg), len_reg);

      vcode_type_t voffset = vtype_offset();
      vcode_var_t i_var = emit_var(voffset, voffset, ident_uniq("i"), 0);
      emit_store(emit_const(voffset, 0), i_var);

      vcode_block_t cmp_bb  = emit_block();
      vcode_block_t body_bb = emit_block();
      vcode_block_t exit_bb = emit_block();

      emit_jump(cmp_bb);

      vcode_select_block(cmp_bb);

      vcode_reg_t i_reg  = emit_load(i_var);
      vcode_reg_t eq_reg = emit_cmp(VCODE_CMP_EQ, i_reg, len_reg);
      emit_cond(eq_reg, exit_bb, body_bb);

      vcode_select_block(body_bb);

      vcode_reg_t ptr_reg = emit_add(subsig, i_reg);
      vcode_reg_t data_reg = lower_array_data(init_reg);
      lower_sub_signals(type_elem(type), where, ptr_reg, data_reg, resolution);

      emit_store(emit_add(i_reg, emit_const(voffset, 1)), i_var);

      emit_jump(cmp_bb);

      vcode_select_block(exit_bb);
   }
   else if (type_is_record(type)) {
      const int nfields = type_fields(type);
      for (int i = 0; i < nfields; i++) {
         type_t ft = tree_type(type_field(type, i));
         vcode_reg_t field_reg = emit_record_ref(init_reg, i);
         vcode_reg_t ptr_reg = emit_record_ref(subsig, i);
         lower_sub_signals(ft, where, ptr_reg, field_reg, resolution);
      }
   }
   else
      fatal_trace("unhandled type %s in lower_sub_signals", type_pp(type));
}

static void lower_signal_decl(tree_t decl)
{
   ident_t name = tree_ident(decl);
   type_t type = tree_type(decl);

   vcode_type_t signal_type = lower_signal_type(type);
   vcode_var_t var;
   if (top_scope->flags & SCOPE_GLOBAL) {
      // This signal may be accessed with the "link var" opcode
      var = emit_var(signal_type, lower_bounds(type), tree_ident2(decl),
                     VAR_SIGNAL | VAR_GLOBAL);
   }
   else
      var = emit_var(signal_type, lower_bounds(type), name, VAR_SIGNAL);
   lower_put_vcode_obj(decl, var, top_scope);

   vcode_reg_t shared, wrapped;
   if (vtype_kind(signal_type) == VCODE_TYPE_UARRAY) {
      shared = emit_link_signal(name, vtype_elem(signal_type));
      wrapped = lower_wrap(type, shared);
   }
   else
      shared = wrapped = emit_link_signal(name, signal_type);

   emit_store(wrapped, var);

   tree_t value = tree_value(decl);
   vcode_reg_t init_reg = lower_expr(value, EXPR_RVALUE);
   if (type_is_array(tree_type(value))) {
      lower_check_array_sizes(decl, type, tree_type(value), wrapped, init_reg);
      init_reg = lower_array_data(init_reg);
   }

   lower_sub_signals(type, decl, shared, init_reg, VCODE_INVALID_REG);
}

static void lower_guard_refs_cb(tree_t ref, void *__ctx)
{
   lower_sched_event(ref, true);
}

static ident_t lower_guard_func(ident_t prefix, tree_t expr)
{
   ident_t qual = ident_prefix(vcode_unit_name(), prefix, '.');
   ident_t func = ident_prefix(qual, ident_new("guard"), '$');

   vcode_state_t state;
   vcode_state_save(&state);

   ident_t context_id = vcode_unit_name();

   emit_function(func, tree_loc(expr), vcode_active_unit());
   vcode_set_result(lower_type(tree_type(expr)));

   vcode_type_t vcontext = vtype_context(context_id);
   emit_param(vcontext, vcontext, ident_new("context"));

   lower_push_scope(NULL);

   tree_visit_only(expr, lower_guard_refs_cb, NULL, T_REF);

   emit_return(lower_reify_expr(expr));

   lower_pop_scope();
   lower_finished();
   vcode_state_restore(&state);

   return func;
}

static void lower_implicit_decl(tree_t decl)
{
   ident_t name = tree_ident(decl);
   type_t type = tree_type(decl);

   vcode_type_t signal_type = lower_signal_type(type);
   vcode_type_t vtype = lower_type(type);
   vcode_type_t vbounds = lower_bounds(type);
   vcode_var_t var = emit_var(signal_type, vbounds, name, VAR_SIGNAL);
   lower_put_vcode_obj(decl, var, top_scope);

   vcode_reg_t shared = emit_link_signal(name, signal_type);
   emit_store(shared, var);

   ident_t func = NULL;
   switch (tree_subkind(decl)) {
   case IMPLICIT_GUARD:
      func = lower_guard_func(tree_ident(decl), tree_value(decl));
      break;
   }

   vcode_reg_t args[] = { lower_context_for_call(func) };
   vcode_reg_t init_reg = emit_fcall(func, vtype, vbounds,
                                     VCODE_CC_VHDL, args, 1);

   vcode_reg_t one_reg = emit_const(vtype_offset(), 1);
   emit_init_signal(shared, init_reg, one_reg, one_reg, VCODE_INVALID_REG);

   vcode_reg_t context_reg = lower_context_for_call(func);
   vcode_reg_t closure =
      emit_closure(func, context_reg, VCODE_INVALID_TYPE, vtype);
   vcode_reg_t kind_reg = emit_const(vtype_offset(), IMPLICIT_GUARD);
   emit_implicit_signal(shared, one_reg, kind_reg, closure);
}

static void lower_file_decl(tree_t decl)
{
   type_t type = tree_type(decl);
   vcode_type_t vtype = lower_type(type);
   const bool is_global = !!(top_scope->flags & SCOPE_GLOBAL);
   ident_t name = is_global ? tree_ident2(decl) : tree_ident(decl);
   vcode_var_t var = emit_var(vtype, vtype, name, is_global ? VAR_GLOBAL : 0);
   lower_put_vcode_obj(decl, var, top_scope);

   emit_store(emit_null(vtype), var);

   if (tree_has_value(decl)) {
      // Generate initial call to file_open

      tree_t value = tree_value(decl);

      vcode_reg_t name_array = lower_expr(tree_value(decl), EXPR_RVALUE);
      vcode_reg_t name_data  = lower_array_data(name_array);
      vcode_reg_t name_len   = lower_array_len(tree_type(value), 0,
                                               name_array);
      vcode_reg_t file_ptr   = emit_index(var, VCODE_INVALID_REG);
      vcode_reg_t mode       = lower_reify_expr(tree_file_mode(decl));

      emit_file_open(file_ptr, name_data, name_len, mode,
                     VCODE_INVALID_REG);
   }
}

static vcode_type_t lower_alias_type(tree_t alias)
{
   type_t type = tree_has_type(alias)
      ? tree_type(alias)
      : tree_type(tree_value(alias));

   if (!type_is_array(type))
      return VCODE_INVALID_TYPE;

   tree_t ref = name_to_ref(tree_value(alias));
   if (ref == NULL || tree_kind(tree_ref(ref)) == T_TYPE_DECL)
      return VCODE_INVALID_TYPE;

   vcode_type_t velem = lower_type(lower_elem_recur(type));
   if (class_of(tree_ref(ref)) == C_SIGNAL)
      velem = vtype_signal(velem);

   vcode_type_t vbounds = lower_bounds(type);
   return vtype_uarray(dimension_of(type), velem, vbounds);
}

static void lower_alias_decl(tree_t decl)
{
   vcode_type_t vtype = lower_alias_type(decl);
   if (vtype == VCODE_INVALID_TYPE)
      return;

   tree_t value = tree_value(decl);
   type_t type = tree_has_type(decl) ? tree_type(decl) : tree_type(value);

   vcode_var_flags_t flags = 0;
   if (!!(top_scope->flags & SCOPE_GLOBAL))
      flags |= VAR_GLOBAL;
   if (class_of(value) == C_SIGNAL)
      flags |= VAR_SIGNAL;

   ident_t name = (flags & VAR_GLOBAL) ? tree_ident2(decl) : tree_ident(decl);

   vcode_var_t var = emit_var(vtype, lower_bounds(type), name, flags);
   lower_put_vcode_obj(decl, var, top_scope);

   const expr_ctx_t ctx = flags & VAR_SIGNAL ? EXPR_LVALUE : EXPR_RVALUE;
   vcode_reg_t value_reg = lower_expr(value, ctx);
   vcode_reg_t data_reg  = lower_array_data(value_reg);

   emit_store(lower_wrap(type, data_reg), var);
}

static void lower_enum_image_helper(type_t type, vcode_reg_t preg)
{
   const int nlits = type_enum_literals(type);
   assert(nlits >= 1);

   vcode_block_t *blocks LOCAL = xmalloc_array(nlits, sizeof(vcode_block_t));
   vcode_reg_t *cases LOCAL = xmalloc_array(nlits, sizeof(vcode_reg_t));

   vcode_type_t vtype = lower_type(type);

   for (int i = 0; i < nlits; i++) {
      cases[i]  = emit_const(vtype, i);
      blocks[i] = emit_block();
   }

   emit_case(preg, blocks[0], cases, blocks, nlits);

   for (int i = 0; i < nlits; i++) {
      // LRM specifies result is lowercase for enumerated types when
      // the value is a basic identifier
      ident_t id = tree_ident(type_enum_literal(type, i));
      if (ident_char(id, 0) != '\'')
         id = ident_downcase(id);

      vcode_select_block(blocks[i]);
      vcode_reg_t str = lower_wrap_string(istr(id));
      emit_return(str);
   }
}

static void lower_physical_image_helper(type_t type, vcode_reg_t preg)
{
   vcode_type_t vchar = vtype_char();
   vcode_type_t strtype = vtype_uarray(1, vchar, vchar);
   vcode_type_t vint64 = vtype_int(INT64_MIN, INT64_MAX);

   vcode_reg_t args[] = { emit_cast(vint64, vint64, preg) };
   vcode_reg_t num_reg = emit_fcall(ident_new("_int_to_string"),
                                    strtype, strtype, VCODE_CC_FOREIGN,
                                    args, 1);
   vcode_reg_t num_len = emit_uarray_len(num_reg, 0);

   const char *unit0 = istr(ident_downcase(tree_ident(type_unit(type, 0))));

   vcode_reg_t append_len = emit_const(vtype_offset(), strlen(unit0) + 1);
   vcode_reg_t total_len = emit_add(num_len, append_len);

   vcode_type_t ctype = vtype_char();
   vcode_reg_t mem_reg = emit_alloca(ctype, ctype, total_len);
   emit_copy(mem_reg, emit_unwrap(num_reg), num_len);

   vcode_reg_t ptr0_reg = emit_add(mem_reg, num_len);
   emit_store_indirect(emit_const(ctype, ' '), ptr0_reg);

   vcode_reg_t unit_reg = lower_wrap_string(unit0);
   vcode_reg_t ptr1_reg = emit_add(ptr0_reg, emit_const(vtype_offset(), 1));
   emit_copy(ptr1_reg, emit_unwrap(unit_reg),
             emit_const(vtype_offset(), strlen(unit0)));

   vcode_dim_t dims[] = {
      { .left  = emit_const(vtype_offset(), 1),
        .right = total_len,
        .dir   = emit_const(vtype_bool(), RANGE_TO)
      }
   };
   emit_return(emit_wrap(mem_reg, dims, 1));
}

static void lower_numeric_image_helper(type_t type, vcode_reg_t preg)
{
   vcode_type_t vchar = vtype_char();
   vcode_type_t vint64 = vtype_int(INT64_MIN, INT64_MAX);
   vcode_type_t strtype = vtype_uarray(1, vchar, vchar);

   vcode_reg_t result;
   if (type_is_real(type)) {
      vcode_reg_t args[] = { preg };
      result = emit_fcall(ident_new("_real_to_string"),
                          strtype, strtype, VCODE_CC_FOREIGN, args, 1);
   }
   else {
      vcode_reg_t args[] = { emit_cast(vint64, vint64, preg) };
      result = emit_fcall(ident_new("_int_to_string"),
                          strtype, strtype, VCODE_CC_FOREIGN, args, 1);
   }

   emit_return(result);
}

static void lower_image_helper(tree_t decl)
{
   type_t type = tree_type(decl);
   const type_kind_t kind = type_kind(type);

   if (kind == T_SUBTYPE)
      return;   // Delegated to base type
   else if (!type_is_scalar(type))
      return;

   ident_t func = ident_prefix(type_ident(type), ident_new("image"), '$');

   vcode_unit_t vu = vcode_find_unit(func);
   if (vu != NULL)
      return;

   vcode_state_t state;
   vcode_state_save(&state);

   ident_t context_id = vcode_unit_name();

   emit_function(func, tree_loc(decl), vcode_active_unit());
   emit_debug_info(tree_loc(decl));

   vcode_type_t ctype = vtype_char();
   vcode_type_t strtype = vtype_uarray(1, ctype, ctype);
   vcode_set_result(strtype);

   vcode_type_t vcontext = vtype_context(context_id);
   emit_param(vcontext, vcontext, ident_new("context"));

   vcode_reg_t preg = emit_param(lower_type(type), lower_bounds(type),
                                 ident_new("VAL"));

   switch (kind) {
   case T_ENUM:
      lower_enum_image_helper(type, preg);
      break;
   case T_INTEGER:
   case T_REAL:
      lower_numeric_image_helper(type, preg);
      break;
   case T_PHYSICAL:
      lower_physical_image_helper(type, preg);
      break;
   default:
      fatal_trace("cannot lower image helper for type %s", type_kind_str(kind));
   }

   lower_finished();
   vcode_state_restore(&state);
}

static vcode_reg_t lower_enum_value_helper(type_t type, vcode_reg_t preg)
{
   const int nlits = type_enum_literals(type);
   assert(nlits >= 1);

   vcode_reg_t arg_len_reg  = emit_uarray_len(preg, 0);
   vcode_reg_t arg_data_reg = emit_unwrap(preg);

   vcode_type_t voffset = vtype_offset();
   vcode_type_t vchar = vtype_char();
   vcode_type_t strtype = vtype_uarray(1, vchar, vchar);

   vcode_reg_t args[] = { arg_data_reg, arg_len_reg };
   vcode_reg_t canon_reg = emit_fcall(ident_new("_canon_value"),
                                      strtype, strtype, VCODE_CC_FOREIGN,
                                      args, 2);
   vcode_reg_t canon_len_reg  = emit_uarray_len(canon_reg, 0);

   size_t stride = 0;
   vcode_reg_t *len_regs LOCAL = xmalloc_array(nlits, sizeof(vcode_reg_t));
   for (int i = 0; i < nlits; i++) {
      size_t len = ident_len(tree_ident(type_enum_literal(type, i)));
      len_regs[i] = emit_const(voffset, len);
      stride = MAX(stride, len);
   }

   vcode_type_t len_array_type = vtype_carray(nlits, voffset, voffset);
   vcode_reg_t len_array_reg =
      emit_const_array(len_array_type, len_regs, nlits);
   vcode_reg_t len_array_ptr = emit_address_of(len_array_reg);

   const size_t nchars = nlits * stride;
   vcode_reg_t *char_regs LOCAL = xmalloc_array(nchars, sizeof(vcode_reg_t));
   for (int i = 0; i < nlits; i++) {
      const char *str = istr(tree_ident(type_enum_literal(type, i)));
      size_t pos = 0;
      for (const char *p = str; *p; p++, pos++)
         char_regs[(i * stride) + pos] = emit_const(vchar, *p);
      for (; pos < stride; pos++)
         char_regs[(i * stride) + pos] = emit_const(voffset, 0);
   }

   vcode_type_t char_array_type = vtype_carray(nlits, vchar, vchar);
   vcode_reg_t char_array_reg =
      emit_const_array(char_array_type, char_regs, nchars);
   vcode_reg_t char_array_ptr = emit_address_of(char_array_reg);

   vcode_var_t i_var = emit_var(voffset, voffset, ident_new("i"), 0);
   emit_store(emit_const(voffset, 0), i_var);

   vcode_block_t head_bb = emit_block();
   vcode_block_t fail_bb = emit_block();
   emit_jump(head_bb);

   const loc_t *loc = vcode_last_loc();

   vcode_select_block(head_bb);

   vcode_reg_t i_reg = emit_load(i_var);

   vcode_block_t memcmp_bb = emit_block();
   vcode_block_t skip_bb   = emit_block();
   vcode_block_t match_bb  = emit_block();

   vcode_reg_t len_ptr = emit_add(len_array_ptr, i_reg);
   vcode_reg_t len_reg = emit_load_indirect(len_ptr);
   vcode_reg_t len_eq  = emit_cmp(VCODE_CMP_EQ, len_reg, canon_len_reg);
   emit_cond(len_eq, memcmp_bb, skip_bb);

   vcode_select_block(memcmp_bb);
   vcode_reg_t char_off = emit_mul(i_reg, emit_const(voffset, stride));
   vcode_reg_t char_ptr = emit_add(char_array_ptr, char_off);

   vcode_dim_t dims[] = {
      { .left  = emit_const(vtype_offset(), 1),
        .right = len_reg,
        .dir   = emit_const(vtype_bool(), RANGE_TO)
      }
   };
   vcode_reg_t str_reg = emit_wrap(char_ptr, dims, 1);

   type_t std_string = std_type(NULL, STD_STRING);
   ident_t func = lower_predef_func_name(std_string, "=");

   vcode_reg_t context_reg = lower_context_for_call(func);
   vcode_reg_t str_cmp_args[] = { context_reg, str_reg, canon_reg };
   vcode_reg_t eq_reg = emit_fcall(func, vtype_bool(), vtype_bool(),
                                   VCODE_CC_PREDEF, str_cmp_args, 3);
   emit_cond(eq_reg, match_bb, skip_bb);

   vcode_select_block(skip_bb);

   vcode_reg_t i_next = emit_add(i_reg, emit_const(voffset, 1));
   emit_store(i_next, i_var);

   vcode_reg_t done_reg = emit_cmp(VCODE_CMP_EQ, i_next,
                                   emit_const(voffset, nlits));
   emit_cond(done_reg, fail_bb, head_bb);

   vcode_select_block(fail_bb);
   emit_debug_info(loc);

   vcode_type_t vseverity = vtype_int(0, SEVERITY_FAILURE - 1);
   vcode_reg_t failure_reg = emit_const(vseverity, SEVERITY_FAILURE);

   vcode_reg_t const_str_reg =
      lower_wrap_string("\" is not a valid enumeration value");
   vcode_reg_t const_str_len = emit_uarray_len(const_str_reg, 0);
   vcode_reg_t extra_len = emit_add(const_str_len, emit_const(voffset, 1));
   vcode_reg_t msg_len = emit_add(arg_len_reg, extra_len);
   vcode_reg_t mem_reg = emit_alloca(vchar, vchar, msg_len);

   emit_store_indirect(emit_const(vchar, '\"'), mem_reg);

   vcode_reg_t ptr1_reg = emit_add(mem_reg, emit_const(voffset, 1));
   emit_copy(ptr1_reg, arg_data_reg, arg_len_reg);

   vcode_reg_t ptr2_reg = emit_add(ptr1_reg, arg_len_reg);
   emit_copy(ptr2_reg, emit_unwrap(const_str_reg), const_str_len);

   emit_report(mem_reg, msg_len, failure_reg);
   emit_return(emit_const(lower_type(type), 0));

   vcode_select_block(match_bb);

   return i_reg;
}

static vcode_reg_t lower_physical_value_helper(type_t type, vcode_reg_t preg)
{
   vcode_reg_t arg_len_reg  = emit_uarray_len(preg, 0);
   vcode_reg_t arg_data_reg = emit_unwrap(preg);

   vcode_type_t voffset = vtype_offset();
   vcode_type_t vchar = vtype_char();
   vcode_type_t vint64 = vtype_int(INT64_MIN, INT64_MAX);
   vcode_type_t strtype = vtype_uarray(1, vchar, vchar);

   vcode_var_t tail_var = emit_var(vtype_pointer(vchar), vchar,
                                   ident_new("tail"), 0);
   vcode_reg_t tail_ptr = emit_index(tail_var, VCODE_INVALID_REG);

   vcode_reg_t args1[] = { arg_data_reg, arg_len_reg, tail_ptr };
   vcode_reg_t int_reg = emit_fcall(ident_new("_string_to_int"),
                                    vint64, vint64, VCODE_CC_FOREIGN, args1, 3);

   vcode_reg_t tail_reg = emit_load_indirect(tail_ptr);
   vcode_reg_t consumed_reg = emit_sub(tail_reg, arg_data_reg);
   vcode_reg_t tail_len = emit_sub(arg_len_reg, consumed_reg);

   vcode_reg_t args2[] = { tail_reg, tail_len };
   vcode_reg_t canon_reg = emit_fcall(ident_new("_canon_value"),
                                      strtype, strtype, VCODE_CC_FOREIGN,
                                      args2, 2);
   vcode_reg_t canon_len_reg  = emit_uarray_len(canon_reg, 0);

   const int nunits = type_units(type);
   assert(nunits >= 1);

   size_t stride = 0;
   vcode_reg_t *len_regs LOCAL = xmalloc_array(nunits, sizeof(vcode_reg_t));
   vcode_reg_t *mul_regs LOCAL = xmalloc_array(nunits, sizeof(vcode_reg_t));
   for (int i = 0; i < nunits; i++) {
      tree_t unit = type_unit(type, i);
      size_t len = ident_len(tree_ident(unit));
      len_regs[i] = emit_const(voffset, len);
      stride = MAX(stride, len);

      vcode_reg_t value_reg = lower_expr(tree_value(unit), EXPR_RVALUE);
      mul_regs[i] = emit_cast(vint64, vint64, value_reg);
   }

   vcode_type_t len_array_type = vtype_carray(nunits, voffset, voffset);
   vcode_reg_t len_array_reg =
      emit_const_array(len_array_type, len_regs, nunits);
   vcode_reg_t len_array_ptr = emit_address_of(len_array_reg);

   vcode_type_t mul_array_type = vtype_carray(nunits, vint64, vint64);
   vcode_reg_t mul_array_reg =
      emit_const_array(mul_array_type, mul_regs, nunits);
   vcode_reg_t mul_array_ptr = emit_address_of(mul_array_reg);

   const size_t nchars = nunits * stride;
   vcode_reg_t *char_regs LOCAL = xmalloc_array(nchars, sizeof(vcode_reg_t));
   for (int i = 0; i < nunits; i++) {
      const char *str = istr(tree_ident(type_unit(type, i)));
      size_t pos = 0;
      for (const char *p = str; *p; p++, pos++)
         char_regs[(i * stride) + pos] = emit_const(vchar, *p);
      for (; pos < stride; pos++)
         char_regs[(i * stride) + pos] = emit_const(voffset, 0);
   }

   vcode_type_t char_array_type = vtype_carray(nunits, vchar, vchar);
   vcode_reg_t char_array_reg =
      emit_const_array(char_array_type, char_regs, nchars);
   vcode_reg_t char_array_ptr = emit_address_of(char_array_reg);

   vcode_var_t i_var = emit_var(voffset, voffset, ident_new("i"), 0);
   emit_store(emit_const(voffset, 0), i_var);

   vcode_block_t head_bb = emit_block();
   vcode_block_t fail_bb = emit_block();
   emit_jump(head_bb);

   const loc_t *loc = vcode_last_loc();

   vcode_select_block(head_bb);

   vcode_reg_t i_reg = emit_load(i_var);

   vcode_block_t memcmp_bb = emit_block();
   vcode_block_t skip_bb   = emit_block();
   vcode_block_t match_bb  = emit_block();

   vcode_reg_t len_ptr = emit_add(len_array_ptr, i_reg);
   vcode_reg_t len_reg = emit_load_indirect(len_ptr);
   vcode_reg_t len_eq  = emit_cmp(VCODE_CMP_EQ, len_reg, canon_len_reg);
   emit_cond(len_eq, memcmp_bb, skip_bb);

   vcode_select_block(memcmp_bb);
   vcode_reg_t char_off = emit_mul(i_reg, emit_const(voffset, stride));
   vcode_reg_t char_ptr = emit_add(char_array_ptr, char_off);

   vcode_dim_t dims[] = {
      { .left  = emit_const(vtype_offset(), 1),
        .right = len_reg,
        .dir   = emit_const(vtype_bool(), RANGE_TO)
      }
   };
   vcode_reg_t str_reg = emit_wrap(char_ptr, dims, 1);

   type_t std_string = std_type(NULL, STD_STRING);
   ident_t func = lower_predef_func_name(std_string, "=");

   vcode_reg_t std_reg = emit_link_package(std_standard_i);
   vcode_reg_t str_cmp_args[] = { std_reg, str_reg, canon_reg };
   vcode_reg_t eq_reg = emit_fcall(func, vtype_bool(), vtype_bool(),
                                   VCODE_CC_PREDEF, str_cmp_args, 3);
   emit_cond(eq_reg, match_bb, skip_bb);

   vcode_select_block(skip_bb);

   vcode_reg_t i_next = emit_add(i_reg, emit_const(voffset, 1));
   emit_store(i_next, i_var);

   vcode_reg_t done_reg = emit_cmp(VCODE_CMP_EQ, i_next,
                                   emit_const(voffset, nunits));
   emit_cond(done_reg, fail_bb, head_bb);

   vcode_select_block(fail_bb);
   emit_debug_info(loc);

   vcode_type_t vseverity = vtype_int(0, SEVERITY_FAILURE - 1);
   vcode_reg_t failure_reg = emit_const(vseverity, SEVERITY_FAILURE);

   vcode_reg_t const_str_reg = lower_wrap_string("\" is not a valid unit name");
   vcode_reg_t const_str_len = emit_uarray_len(const_str_reg, 0);
   vcode_reg_t extra_len = emit_add(const_str_len, emit_const(voffset, 1));
   vcode_reg_t msg_len = emit_add(tail_len, extra_len);
   vcode_reg_t mem_reg = emit_alloca(vchar, vchar, msg_len);

   emit_store_indirect(emit_const(vchar, '\"'), mem_reg);

   vcode_reg_t ptr1_reg = emit_add(mem_reg, emit_const(voffset, 1));
   emit_copy(ptr1_reg, tail_reg, tail_len);

   vcode_reg_t ptr2_reg = emit_add(ptr1_reg, tail_len);
   emit_copy(ptr2_reg, emit_unwrap(const_str_reg), const_str_len);

   emit_report(mem_reg, msg_len, failure_reg);
   emit_return(emit_const(lower_type(type), 0));

   vcode_select_block(match_bb);

   vcode_reg_t mul_ptr = emit_add(mul_array_ptr, i_reg);
   vcode_reg_t mul_reg = emit_load_indirect(mul_ptr);
   return emit_mul(int_reg, mul_reg);
}

static vcode_reg_t lower_numeric_value_helper(type_t type, vcode_reg_t preg)
{
   vcode_type_t vchar = vtype_char();
   vcode_type_t vint64 = vtype_int(INT64_MIN, INT64_MAX);
   vcode_type_t vreal  = vtype_real();

   vcode_reg_t len_reg  = emit_uarray_len(preg, 0);
   vcode_reg_t data_reg = emit_unwrap(preg);
   vcode_reg_t null_reg = emit_null(vtype_pointer(vtype_pointer(vchar)));

   vcode_reg_t args[] = { data_reg, len_reg, null_reg };

   if (type_is_real(type))
      return emit_fcall(ident_new("_string_to_real"),
                        vreal, vreal, VCODE_CC_FOREIGN, args, 3);
   else
      return emit_fcall(ident_new("_string_to_int"),
                        vint64, vint64, VCODE_CC_FOREIGN, args, 3);
}

static void lower_value_helper(tree_t decl)
{
   type_t type = tree_type(decl);
   const type_kind_t kind = type_kind(type);

   if (kind == T_SUBTYPE)
      return;   // Delegated to base type
   else if (!type_is_scalar(type))
      return;

   ident_t func = ident_prefix(type_ident(type), ident_new("value"), '$');

   vcode_unit_t vu = vcode_find_unit(func);
   if (vu != NULL)
      return;

   vcode_state_t state;
   vcode_state_save(&state);

   ident_t context_id = vcode_unit_name();

   emit_function(func, tree_loc(decl), vcode_active_unit());
   vcode_set_result(lower_type(type));

   vcode_type_t vcontext = vtype_context(context_id);
   emit_param(vcontext, vcontext, ident_new("context"));

   vcode_type_t ctype = vtype_char();
   vcode_type_t strtype = vtype_uarray(1, ctype, ctype);
   vcode_reg_t preg = emit_param(strtype, strtype, ident_new("VAL"));

   vcode_reg_t result = VCODE_INVALID_REG;
   switch (kind) {
   case T_ENUM:
      result = lower_enum_value_helper(type, preg);
      break;
   case T_INTEGER:
   case T_REAL:
      result = lower_numeric_value_helper(type, preg);
      break;
   case T_PHYSICAL:
      result = lower_physical_value_helper(type, preg);
      break;
   default:
      fatal_trace("cannot lower value helper for type %s", type_kind_str(kind));
   }

   lower_check_scalar_bounds(result, type, decl, NULL);;
   emit_return(emit_cast(lower_type(type), lower_bounds(type), result));

   lower_finished();
   vcode_state_restore(&state);
}

static void lower_decl(tree_t decl)
{
   PUSH_DEBUG_INFO(decl);

   switch (tree_kind(decl)) {
   case T_CONST_DECL:
   case T_VAR_DECL:
      lower_var_decl(decl);
      break;

   case T_SIGNAL_DECL:
      lower_signal_decl(decl);
      break;

   case T_IMPLICIT_SIGNAL:
      lower_implicit_decl(decl);
      break;

   case T_FILE_DECL:
      lower_file_decl(decl);
      break;

   case T_ALIAS:
      lower_alias_decl(decl);
      break;

   case T_HIER:
      top_scope->hier = decl;
      break;

   case T_TYPE_DECL:
      lower_image_helper(decl);
      lower_value_helper(decl);
      break;

   case T_FUNC_DECL:
   case T_PROC_DECL:
   case T_ATTR_SPEC:
   case T_ATTR_DECL:
   case T_COMPONENT:
   case T_USE:
   case T_SPEC:
   case T_GROUP:
   case T_GROUP_TEMPLATE:
      break;

   default:
      fatal_trace("cannot lower decl kind %s", tree_kind_str(tree_kind(decl)));
   }
}

static void lower_finished(void)
{
   vcode_opt();

   if (verbose != NULL) {
      if (*verbose == '\0' || strstr(istr(vcode_unit_name()), verbose) != NULL)
         vcode_dump();
   }
}

static void lower_protected_body(tree_t body, vcode_unit_t context)
{
   vcode_select_unit(context);

   type_t type = tree_type(body);
   vcode_unit_t vu = emit_protected(type_ident(type), tree_loc(body), context);

   lower_push_scope(body);

   lower_decls(body, vu);
   emit_return(VCODE_INVALID_REG);

   lower_finished();
   lower_pop_scope();
}

static void lower_decls(tree_t scope, vcode_unit_t context)
{
   // Lower declarations in two passes with subprograms after signals,
   // variables, constants, etc.

   const int ndecls = tree_decls(scope);

   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(scope, i);
      const tree_kind_t kind = tree_kind(d);
      if (mode == LOWER_THUNK && kind == T_SIGNAL_DECL)
         continue;
      else if (is_subprogram(d) || kind == T_PROT_BODY)
         continue;
      else
         lower_decl(d);
   }

   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(scope, i);
      const tree_kind_t kind = tree_kind(d);
      if (kind != T_FUNC_BODY && kind != T_PROC_BODY && kind != T_PROT_BODY
          && kind != T_FUNC_DECL)
         continue;

      vcode_block_t bb = vcode_active_block();

      if (kind == T_PROT_BODY && mode == LOWER_THUNK)
         continue;

      switch (kind) {
      case T_FUNC_BODY: lower_func_body(d, context); break;
      case T_PROC_BODY: lower_proc_body(d, context); break;
      case T_PROT_BODY: lower_protected_body(d, context); break;
      case T_FUNC_DECL: lower_predef(d, context); break;
      default: break;
      }

      vcode_select_unit(context);
      vcode_select_block(bb);
   }
}

static bool lower_has_subprograms(tree_t scope)
{
   const int ndecls = tree_decls(scope);
   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(scope, i);
      const tree_kind_t kind = tree_kind(d);
      if (kind == T_FUNC_BODY || kind == T_PROC_BODY)
         return true;
      else if (kind == T_TYPE_DECL) {
         // Predefined operators for certain types may reference the
         // parameters: e.g. an array with non-static length
         type_t type = tree_type(d);
         if (type_kind(type) == T_SUBTYPE)
            continue;
         else if (type_is_record(type) || type_is_array(type))
            return true;
      }
   }

   return false;
}

static void lower_subprogram_ports(tree_t body, bool params_as_vars)
{
   const int nports = tree_ports(body);
   for (int i = 0; i < nports; i++) {
      tree_t p = tree_port(body, i);
      type_t type = tree_type(p);

      vcode_type_t vtype, vbounds;
      switch (tree_class(p)) {
      case C_SIGNAL:
         vtype = lower_signal_type(type);
         vbounds = vtype;
         break;

      case C_VARIABLE:
      case C_DEFAULT:
      case C_CONSTANT:
         {
            if (type_is_array(type) && lower_const_bounds(type)) {
               type_t elem = lower_elem_recur(type);
               vtype = vtype_pointer(lower_type(elem));
               vbounds = lower_bounds(elem);
            }
            else if (type_is_record(type)) {
               vtype = vtype_pointer(lower_type(type));
               vbounds = vtype;
            }
            else {
               vtype = lower_type(type);
               vbounds = lower_bounds(type);
            }

            const port_mode_t mode = tree_subkind(p);
            if ((mode == PORT_OUT || mode == PORT_INOUT)
                && !type_is_array(type) && !type_is_record(type))
               vtype = vtype_pointer(vtype);
         }
         break;

      case C_FILE:
         vtype = vtype_pointer(lower_type(type));
         vbounds = vtype;
         break;

      default:
         fatal_trace("unhandled class %s in lower_subprogram_ports",
                     class_str(tree_class(p)));
      }

      vcode_reg_t preg = emit_param(vtype, vbounds, tree_ident(p));
      if (params_as_vars) {
         vcode_var_t var = emit_var(vtype, vbounds, tree_ident(p), 0);
         emit_store(preg, var);
         lower_put_vcode_obj(p, var | 0x20000000, top_scope);
      }
      else
         lower_put_vcode_obj(p, preg, top_scope);
   }
}

static ident_t lower_predef_func_name(type_t type, const char *op)
{
   type_t base = type_base_recur(type);

   LOCAL_TEXT_BUF tb = tb_new();
   tb_printf(tb, "%s.\"%s\"(", istr(ident_runtil(type_ident(base), '.')), op);
   mangle_one_type(tb, base);
   mangle_one_type(tb, base);
   tb_cat(tb, ")");
   mangle_one_type(tb, std_type(NULL, STD_BOOLEAN));

   return ident_new(tb_get(tb));
}

static void lower_array_cmp_inner(vcode_reg_t lhs_data,
                                  vcode_reg_t rhs_data,
                                  vcode_reg_t lhs_array,
                                  vcode_reg_t rhs_array,
                                  type_t left_type,
                                  type_t right_type,
                                  vcode_cmp_t pred,
                                  vcode_block_t fail_bb)
{
   // Behaviour of relational operators on arrays is described in
   // LRM 93 section 7.2.2

   assert(pred == VCODE_CMP_EQ || pred == VCODE_CMP_LT
          || pred == VCODE_CMP_LEQ);

   const int ndims = dimension_of(left_type);
   assert(dimension_of(right_type) == ndims);

   vcode_reg_t left_len = lower_array_len(left_type, 0, lhs_array);
   for (int i = 1; i < ndims; i++) {
      vcode_reg_t dim_len = lower_array_len(left_type, i, lhs_array);
      left_len = emit_mul(dim_len, left_len);
   }

   vcode_reg_t right_len = lower_array_len(right_type, 0, rhs_array);
   for (int i = 1; i < ndims; i++) {
      vcode_reg_t dim_len = lower_array_len(right_type, i, rhs_array);
      right_len = emit_mul(dim_len, right_len);
   }

   vcode_type_t voffset = vtype_offset();
   vcode_var_t i_var = emit_var(voffset, voffset, ident_uniq("i"), 0);
   emit_store(emit_const(voffset, 0), i_var);

   vcode_block_t test_bb = emit_block();
   vcode_block_t body_bb = emit_block();
   vcode_block_t exit_bb = emit_block();

   type_t elem_type = type_elem(left_type);

   vcode_reg_t stride = VCODE_INVALID_REG;
   if (type_is_array(elem_type))
      stride = lower_array_total_len(elem_type, VCODE_INVALID_REG);

   vcode_reg_t len_eq = emit_cmp(VCODE_CMP_EQ, left_len, right_len);

   if (pred == VCODE_CMP_EQ)
      emit_cond(len_eq, test_bb, fail_bb);
   else
      emit_jump(test_bb);

   // Loop test

   vcode_select_block(test_bb);

   vcode_reg_t i_loaded = emit_load(i_var);

   if (pred == VCODE_CMP_EQ) {
      vcode_reg_t done = emit_cmp(VCODE_CMP_EQ, i_loaded, left_len);
      emit_cond(done, exit_bb, body_bb);
   }
   else {
      vcode_block_t check_r_len_bb = emit_block();

      vcode_reg_t len_ge_l = emit_cmp(VCODE_CMP_GEQ, i_loaded, left_len);
      emit_cond(len_ge_l, exit_bb, check_r_len_bb);

      vcode_select_block(check_r_len_bb);

      vcode_reg_t len_ge_r = emit_cmp(VCODE_CMP_GEQ, i_loaded, right_len);
      emit_cond(len_ge_r, fail_bb, body_bb);
   }

   // Loop body

   vcode_select_block(body_bb);

   vcode_reg_t ptr_inc = i_loaded;
   if (stride != VCODE_INVALID_REG)
      ptr_inc = emit_mul(ptr_inc, stride);

   vcode_reg_t inc = emit_add(i_loaded, emit_const(voffset, 1));
   emit_store(inc, i_var);

   vcode_reg_t i_eq_len = emit_cmp(VCODE_CMP_EQ, inc, left_len);

   vcode_reg_t l_ptr = emit_add(lhs_data, ptr_inc);
   vcode_reg_t r_ptr = emit_add(rhs_data, ptr_inc);

   if (type_is_array(elem_type)) {
      lower_array_cmp_inner(l_ptr, r_ptr,
                            VCODE_INVALID_REG, VCODE_INVALID_REG,
                            type_elem(left_type), type_elem(right_type),
                            pred, fail_bb);
      emit_jump(test_bb);
   }
   else if (type_is_record(elem_type)) {
      ident_t func = lower_predef_func_name(elem_type, "=");
      vcode_reg_t context_reg = lower_context_for_call(func);
      vcode_reg_t args[] = { context_reg, l_ptr, r_ptr };
      vcode_type_t vbool = vtype_bool();
      vcode_reg_t eq = emit_fcall(func, vbool, vbool, VCODE_CC_PREDEF, args, 3);
      emit_cond(eq, test_bb, fail_bb);
   }
   else {
      vcode_reg_t l_val = emit_load_indirect(l_ptr);
      vcode_reg_t r_val = emit_load_indirect(r_ptr);

      if (pred == VCODE_CMP_EQ) {
         vcode_reg_t eq = emit_cmp(pred, l_val, r_val);
         emit_cond(eq, test_bb, fail_bb);
      }
      else {
         vcode_reg_t cmp = emit_cmp(pred, l_val, r_val);
         vcode_reg_t eq  = emit_cmp(VCODE_CMP_EQ, l_val, r_val);

         vcode_reg_t done = emit_or(emit_not(eq), emit_and(len_eq, i_eq_len));

         vcode_block_t cmp_result_bb = emit_block();
         emit_cond(done, cmp_result_bb, test_bb);

         vcode_select_block(cmp_result_bb);
         emit_cond(cmp, exit_bb, fail_bb);
      }
   }

   // Epilogue

   vcode_select_block(exit_bb);
}

static void lower_predef_array_cmp(tree_t decl, vcode_unit_t context,
                                   vcode_cmp_t pred)
{
   type_t r0_type = tree_type(tree_port(decl, 0));
   type_t r1_type = tree_type(tree_port(decl, 1));

   vcode_reg_t r0 = 1, r1 = 2;
   vcode_reg_t r0_data = lower_array_data(r0);
   vcode_reg_t r1_data = lower_array_data(r1);

   vcode_block_t fail_bb = emit_block();

   lower_array_cmp_inner(r0_data, r1_data, r0, r1, r0_type, r1_type,
                         pred, fail_bb);

   emit_return(emit_const(vtype_bool(), 1));

   vcode_select_block(fail_bb);
   emit_return(emit_const(vtype_bool(), 0));
}

static void lower_predef_record_eq(tree_t decl, vcode_unit_t context)
{
   vcode_reg_t r0 = 1, r1 = 2;
   type_t type = tree_type(tree_port(decl, 0));

   vcode_block_t fail_bb = emit_block();

   const int nfields = type_fields(type);
   for (int i = 0; i < nfields; i++) {
      vcode_reg_t lfield = emit_record_ref(r0, i);
      vcode_reg_t rfield = emit_record_ref(r1, i);

      vcode_reg_t cmp = VCODE_INVALID_REG;
      type_t ftype = tree_type(type_field(type, i));
      if (type_is_array(ftype)) {
         ident_t func = lower_predef_func_name(ftype, "=");

         vcode_reg_t args[3];
         args[0] = lower_context_for_call(func);
         if (!lower_const_bounds(ftype)) {
            // Have pointers to uarrays
            args[1] = emit_load_indirect(lfield);
            args[2] = emit_load_indirect(rfield);
         }
         else {
            args[1] = lower_wrap(ftype, lfield);
            args[2] = lower_wrap(ftype, rfield);
         }

         vcode_type_t vbool = vtype_bool();
         cmp = emit_fcall(func, vbool, vbool, VCODE_CC_PREDEF, args, 3);
      }
      else if (type_is_record(ftype)) {
         ident_t func = lower_predef_func_name(ftype, "=");
         vcode_reg_t context_reg = lower_context_for_call(func);
         vcode_reg_t args[] = { context_reg, lfield, rfield };
         vcode_type_t vbool = vtype_bool();
         cmp = emit_fcall(func, vbool, vbool, VCODE_CC_PREDEF, args, 3);
      }
      else {
         vcode_reg_t lload = emit_load_indirect(lfield);
         vcode_reg_t rload = emit_load_indirect(rfield);
         cmp = emit_cmp(VCODE_CMP_EQ, lload, rload);
      }

      vcode_block_t next_bb = emit_block();
      emit_cond(cmp, next_bb, fail_bb);
      vcode_select_block(next_bb);
   }

   emit_return(emit_const(vtype_bool(), 1));

   vcode_select_block(fail_bb);
   emit_return(emit_const(vtype_bool(), 0));
}

static void lower_predef_scalar_to_string(type_t arg_type, type_t std_string,
                                          vcode_unit_t context)
{
   // LRM 08 section 5.7 on string representations

   ident_t func = ident_prefix(type_ident(arg_type), ident_new("image"), '$');
   vcode_type_t rtype = lower_type(std_string);
   vcode_type_t rbounds = lower_bounds(std_string);
   vcode_reg_t context_reg = 0, r0 = 1;
   vcode_reg_t args[] = { context_reg, r0 };
   vcode_reg_t str_reg =
      emit_fcall(func, rtype, rbounds, VCODE_CC_PREDEF, args, 2);

   if (type_is_enum(arg_type)) {
      // If the result is a character literal return just the character
      // without the quotes
      vcode_reg_t quote_reg = emit_const(vtype_char(), '\'');
      vcode_reg_t data_reg  = lower_array_data(str_reg);
      vcode_reg_t char0_reg = emit_load_indirect(data_reg);
      vcode_reg_t is_quote  = emit_cmp(VCODE_CMP_EQ, char0_reg, quote_reg);

      vcode_block_t char_bb  = emit_block();
      vcode_block_t other_bb = emit_block();

      emit_cond(is_quote, char_bb, other_bb);

      vcode_select_block(char_bb);

      vcode_reg_t char1_ptr = emit_add(data_reg, emit_const(vtype_offset(), 1));
      vcode_reg_t left_reg  = emit_uarray_left(str_reg, 0);
      vcode_reg_t dir_reg   = emit_uarray_dir(str_reg, 0);

      vcode_dim_t dims[] = {
         { .left  = left_reg,
           .right = left_reg,
           .dir   = dir_reg
         }
      };
      emit_return(emit_wrap(char1_ptr, dims, 1));

      vcode_select_block(other_bb);

      emit_return(str_reg);
   }
   else
      emit_return(str_reg);
}

static void lower_predef_array_to_string(type_t arg_type, type_t std_string,
                                         vcode_unit_t context)
{

   type_t arg_elem    = type_base_recur(type_elem(arg_type));
   type_t result_elem = type_base_recur(type_elem(std_string));

   vcode_type_t elem_vtype = lower_type(result_elem);

   const int nlits = type_enum_literals(arg_elem);
   vcode_reg_t *map LOCAL = xmalloc_array(nlits, sizeof(vcode_reg_t));
   for (int i = 0; i < nlits; i++) {
      const ident_t id = tree_ident(type_enum_literal(arg_elem, i));
      assert(ident_char(id, 0) == '\'');
      map[i] = emit_const(elem_vtype, ident_char(id, 1));
   }

   vcode_reg_t array_reg = 1;

   vcode_type_t map_vtype = vtype_carray(nlits, elem_vtype, elem_vtype);
   vcode_reg_t map_reg = emit_const_array(map_vtype, map, nlits);

   vcode_reg_t len_reg = lower_array_len(arg_type, 0, array_reg);
   vcode_reg_t mem_reg = emit_alloca(elem_vtype, elem_vtype, len_reg);

   vcode_type_t index_vtype = lower_type(index_type_of(std_string, 0));

   vcode_reg_t left_reg  = lower_array_left(arg_type, 0, array_reg);
   vcode_reg_t right_reg = lower_array_right(arg_type, 0, array_reg);
   vcode_reg_t dir_reg   = lower_array_dir(arg_type, 0, array_reg);

   ident_t i_name = ident_uniq("to_string_i");
   vcode_var_t i_var = emit_var(vtype_offset(), vtype_offset(), i_name, 0);
   emit_store(emit_const(vtype_offset(), 0), i_var);

   vcode_reg_t null_reg = emit_range_null(left_reg, right_reg, dir_reg);

   vcode_block_t body_bb = emit_block();
   vcode_block_t exit_bb = emit_block();

   emit_cond(null_reg, exit_bb, body_bb);

   vcode_select_block(body_bb);

   vcode_reg_t i_reg    = emit_load(i_var);
   vcode_reg_t sptr_reg = emit_add(lower_array_data(array_reg), i_reg);
   vcode_reg_t src_reg  = emit_load_indirect(sptr_reg);
   vcode_reg_t off_reg  = emit_cast(vtype_offset(), vtype_offset(), src_reg);
   vcode_reg_t lptr_reg = emit_add(emit_address_of(map_reg), off_reg);
   vcode_reg_t dptr_reg = emit_add(lower_array_data(mem_reg), i_reg);
   emit_store_indirect(emit_load_indirect(lptr_reg), dptr_reg);

   vcode_reg_t next_reg = emit_add(i_reg, emit_const(vtype_offset(), 1));
   vcode_reg_t cmp_reg  = emit_cmp(VCODE_CMP_EQ, next_reg, len_reg);
   emit_store(next_reg, i_var);
   emit_cond(cmp_reg, exit_bb, body_bb);

   vcode_select_block(exit_bb);

   vcode_dim_t dims[] = {
      {
         .left  = emit_const(index_vtype, 1),
         .right = emit_cast(index_vtype, index_vtype, len_reg),
         .dir   = emit_const(vtype_bool(), RANGE_TO)
      }
   };
   emit_return(emit_wrap(mem_reg, dims, 1));
}

static void lower_predef_to_string(tree_t decl, vcode_unit_t context)
{
   type_t arg_type = tree_type(tree_port(decl, 0));
   type_t result_type = type_result(tree_type(decl));

   if (type_is_scalar(arg_type))
      lower_predef_scalar_to_string(arg_type, result_type, context);
   else if (type_is_array(arg_type))
      lower_predef_array_to_string(arg_type, result_type, context);
   else
      fatal_trace("cannot generate TO_STRING for %s", type_pp(arg_type));
}

static void lower_predef_bit_shift(tree_t decl, vcode_unit_t context,
                                   subprogram_kind_t kind)
{
   type_t type = tree_type(tree_port(decl, 0));
   type_t elem = type_elem(type);

   vcode_type_t vtype = lower_type(elem);
   vcode_type_t vbounds = lower_bounds(elem);
   vcode_type_t voffset = vtype_offset();

   vcode_reg_t r0 = 1, r1 = 2;

   vcode_reg_t data_reg = lower_array_data(r0);
   vcode_reg_t len_reg  = lower_array_len(type, 0, r0);

   vcode_block_t null_bb = emit_block();
   vcode_block_t non_null_bb = emit_block();

   vcode_reg_t is_null_reg =
      emit_cmp(VCODE_CMP_EQ, len_reg, emit_const(voffset, 0));
   emit_cond(is_null_reg, null_bb, non_null_bb);

   vcode_select_block(null_bb);
   emit_return(r0);

   vcode_select_block(non_null_bb);

   vcode_reg_t shift_reg = emit_cast(vtype_offset(), VCODE_INVALID_TYPE, r1);
   vcode_reg_t mem_reg = emit_alloca(vtype, vbounds, len_reg);

   vcode_var_t i_var = emit_var(voffset, voffset, ident_uniq("i"), 0);
   emit_store(emit_const(voffset, 0), i_var);

   vcode_block_t cmp_bb  = emit_block();
   vcode_block_t body_bb = emit_block();
   vcode_block_t exit_bb = emit_block();

   vcode_reg_t def_reg = VCODE_INVALID_REG;
   switch (kind) {
   case S_SLL: case S_SRL: case S_ROL: case S_ROR:
      def_reg = emit_const(vtype, 0);
      break;
   case S_SRA:
      {
         vcode_reg_t last_ptr =
            emit_add(data_reg, emit_sub(len_reg, emit_const(voffset, 1)));
         def_reg = emit_load_indirect(last_ptr);
      }
      break;
   case S_SLA:
      def_reg = emit_load_indirect(data_reg);
      break;
   default:
      break;
   }

   vcode_reg_t shift_is_neg =
      emit_cmp(VCODE_CMP_LT, shift_reg, emit_const(voffset, 0));

   emit_jump(cmp_bb);

   vcode_select_block(cmp_bb);

   vcode_reg_t i_reg  = emit_load(i_var);
   vcode_reg_t eq_reg = emit_cmp(VCODE_CMP_EQ, i_reg, len_reg);
   emit_cond(eq_reg, exit_bb, body_bb);

   vcode_select_block(body_bb);

   vcode_reg_t cmp_reg = VCODE_INVALID_REG;
   switch (kind) {
   case S_SRL: case S_SRA:
      {
         vcode_reg_t neg_reg =
            emit_cmp(VCODE_CMP_LT, i_reg, emit_add(len_reg, shift_reg));
         vcode_reg_t pos_reg =
            emit_cmp(VCODE_CMP_GEQ, i_reg, shift_reg);
         cmp_reg = emit_select(shift_is_neg, neg_reg, pos_reg);
      }
      break;
   case S_SLL: case S_SLA:
      {
         vcode_reg_t neg_reg =
            emit_cmp(VCODE_CMP_GEQ, i_reg, emit_neg(shift_reg));
         vcode_reg_t pos_reg =
            emit_cmp(VCODE_CMP_LT, i_reg, emit_sub(len_reg, shift_reg));
         cmp_reg = emit_select(shift_is_neg, neg_reg, pos_reg);
      }
      break;
   case S_ROL: case S_ROR:
      cmp_reg = emit_const(vtype_bool(), 1);
   default:
      break;
   }

   vcode_reg_t dst_ptr = emit_add(mem_reg, i_reg);

   vcode_reg_t next_reg = emit_add(i_reg, emit_const(vtype_offset(), 1));
   emit_store(next_reg, i_var);

   vcode_block_t true_bb = emit_block();
   vcode_block_t false_bb = emit_block();

   emit_cond(cmp_reg, true_bb, false_bb);

   vcode_select_block(true_bb);

   vcode_reg_t src_reg = VCODE_INVALID_REG;
   switch (kind) {
   case S_SLL: case S_SLA:
      src_reg = emit_add(i_reg, shift_reg);
      break;
   case S_SRL: case S_SRA:
      src_reg = emit_sub(i_reg, shift_reg);
      break;
   case S_ROL:
      src_reg = emit_mod(emit_add(i_reg, emit_add(len_reg, shift_reg)),
                         len_reg);
      break;
   case S_ROR:
      src_reg = emit_mod(emit_add(i_reg, emit_sub(len_reg, shift_reg)),
                         len_reg);
      break;
   default:
      break;
   }

   vcode_reg_t load_reg = emit_load_indirect(emit_add(data_reg, src_reg));
   emit_store_indirect(load_reg, dst_ptr);
   emit_jump(cmp_bb);

   vcode_select_block(false_bb);
   emit_store_indirect(def_reg, dst_ptr);
   emit_jump(cmp_bb);

   vcode_select_block(exit_bb);

   vcode_reg_t left_reg  = emit_uarray_left(r0, 0);
   vcode_reg_t right_reg = emit_uarray_right(r0, 0);
   vcode_reg_t dir_reg   = emit_uarray_dir(r0, 0);

   vcode_dim_t dims[] = { { left_reg, right_reg, dir_reg } };
   emit_return(emit_wrap(mem_reg, dims, 1));
}

static void lower_predef_bit_vec_op(tree_t decl, vcode_unit_t context,
                                    subprogram_kind_t kind)
{
   type_t type = tree_type(tree_port(decl, 0));
   type_t elem = type_elem(type);

   vcode_type_t vtype = lower_type(elem);
   vcode_type_t vbounds = lower_bounds(elem);
   vcode_type_t voffset = vtype_offset();

   vcode_reg_t r0 = 1, r1 = 2;

   vcode_reg_t data0_reg = lower_array_data(r0);
   vcode_reg_t data1_reg = VCODE_INVALID_REG;
   if (kind != S_ARRAY_NOT)
      data1_reg = lower_array_data(r1);

   vcode_reg_t len0_reg = lower_array_len(type, 0, r0);
   vcode_reg_t len1_reg = VCODE_INVALID_REG;
   if (kind != S_ARRAY_NOT) {
      len1_reg = lower_array_len(type, 0, r1);

      vcode_block_t fail_bb = emit_block();
      vcode_block_t cont_bb = emit_block();

      vcode_reg_t len_eq = emit_cmp(VCODE_CMP_EQ, len0_reg, len1_reg);
      emit_cond(len_eq, cont_bb, fail_bb);

      vcode_select_block(fail_bb);

      vcode_type_t vseverity = vtype_int(0, SEVERITY_FAILURE - 1);
      vcode_reg_t failure_reg = emit_const(vseverity, SEVERITY_FAILURE);

      vcode_reg_t msg_reg =
         lower_wrap_string("arguments have different lengths");
      vcode_reg_t msg_len = emit_uarray_len(msg_reg, 0);

      emit_debug_info(tree_loc(decl));
      emit_report(emit_unwrap(msg_reg), msg_len, failure_reg);
      emit_return(r0);

      vcode_select_block(cont_bb);
   }

   vcode_reg_t mem_reg = emit_alloca(vtype, vbounds, len0_reg);

   vcode_var_t i_var = emit_var(voffset, voffset, ident_uniq("i"), 0);
   emit_store(emit_const(voffset, 0), i_var);

   vcode_block_t cmp_bb  = emit_block();
   vcode_block_t body_bb = emit_block();
   vcode_block_t exit_bb = emit_block();

   emit_jump(cmp_bb);

   vcode_select_block(cmp_bb);

   vcode_reg_t i_reg  = emit_load(i_var);
   vcode_reg_t eq_reg = emit_cmp(VCODE_CMP_EQ, i_reg, len0_reg);
   emit_cond(eq_reg, exit_bb, body_bb);

   vcode_select_block(body_bb);

   vcode_reg_t dst_ptr = emit_add(mem_reg, i_reg);

   vcode_reg_t src0_reg = emit_load_indirect(emit_add(data0_reg, i_reg));
   vcode_reg_t src1_reg = VCODE_INVALID_REG;
   if (kind != S_ARRAY_NOT)
      src1_reg = emit_load_indirect(emit_add(data1_reg, i_reg));

   vcode_reg_t op_reg;
   switch (kind) {
   case S_ARRAY_NOT:  op_reg = emit_not(src0_reg); break;
   case S_ARRAY_AND:  op_reg = emit_and(src0_reg, src1_reg); break;
   case S_ARRAY_OR:   op_reg = emit_or(src0_reg, src1_reg); break;
   case S_ARRAY_XOR:  op_reg = emit_xor(src0_reg, src1_reg); break;
   case S_ARRAY_XNOR: op_reg = emit_xnor(src0_reg, src1_reg); break;
   case S_ARRAY_NAND: op_reg = emit_nand(src0_reg, src1_reg); break;
   case S_ARRAY_NOR:  op_reg = emit_nor(src0_reg, src1_reg); break;
   default:
      fatal_trace("unhandled bitvec operator kind %d", kind);
   }

   emit_store_indirect(op_reg, dst_ptr);

   vcode_reg_t next_reg = emit_add(i_reg, emit_const(vtype_offset(), 1));
   emit_store(next_reg, i_var);
   emit_jump(cmp_bb);

   vcode_select_block(exit_bb);

   vcode_reg_t left_reg  = emit_uarray_left(r0, 0);
   vcode_reg_t right_reg = emit_uarray_right(r0, 0);
   vcode_reg_t dir_reg   = emit_uarray_dir(r0, 0);

   vcode_dim_t dims[] = { { left_reg, right_reg, dir_reg } };
   emit_return(emit_wrap(mem_reg, dims, 1));
}

static void lower_predef_mixed_bit_vec_op(tree_t decl, vcode_unit_t context,
                                          subprogram_kind_t kind)
{
   // Mixed scalar/array bit vector operations

   vcode_reg_t r0 = 1, r1 = 2;

   type_t r0_type = tree_type(tree_port(decl, 0));
   type_t r1_type = tree_type(tree_port(decl, 1));

   vcode_type_t voffset = vtype_offset();

   vcode_var_t i_var = emit_var(voffset, voffset, ident_uniq("i"), 0);
   emit_store(emit_const(vtype_offset(), 0), i_var);

   const bool r0_is_array = type_is_array(r0_type);

   type_t array_type = r0_is_array ? r0_type : r1_type;
   vcode_reg_t array_reg = r0_is_array ? r0 : r1;

   vcode_reg_t len_reg   = lower_array_len(array_type, 0, array_reg);
   vcode_reg_t data_reg  = lower_array_data(array_reg);
   vcode_reg_t left_reg  = lower_array_left(array_type, 0, array_reg);
   vcode_reg_t right_reg = lower_array_right(array_type, 0, array_reg);
   vcode_reg_t dir_reg   = lower_array_dir(array_type, 0, array_reg);
   vcode_reg_t null_reg  = emit_range_null(left_reg, right_reg, dir_reg);

   vcode_reg_t mem_reg = emit_alloca(vtype_bool(), vtype_bool(), len_reg);

   vcode_block_t body_bb = emit_block();
   vcode_block_t exit_bb = emit_block();

   emit_cond(null_reg, exit_bb, body_bb);

   vcode_select_block(body_bb);

   vcode_reg_t i_reg = emit_load(i_var);
   vcode_reg_t l_reg = emit_load_indirect(emit_add(data_reg, i_reg));
   vcode_reg_t r_reg = r0_is_array ? r1 : r0;

   vcode_reg_t result_reg = VCODE_INVALID_REG;
   switch (kind) {
   case S_MIXED_AND:  result_reg = emit_and(l_reg, r_reg);  break;
   case S_MIXED_OR:   result_reg = emit_or(l_reg, r_reg);   break;
   case S_MIXED_NAND: result_reg = emit_nand(l_reg, r_reg); break;
   case S_MIXED_NOR:  result_reg = emit_nor(l_reg, r_reg);  break;
   case S_MIXED_XOR:  result_reg = emit_xor(l_reg, r_reg);  break;
   case S_MIXED_XNOR: result_reg = emit_xnor(l_reg, r_reg); break;
   default: break;
   }

   emit_store_indirect(result_reg, emit_add(mem_reg, i_reg));

   vcode_reg_t next_reg = emit_add(i_reg, emit_const(voffset, 1));
   vcode_reg_t cmp_reg  = emit_cmp(VCODE_CMP_EQ, next_reg, len_reg);
   emit_store(next_reg, i_var);
   emit_cond(cmp_reg, exit_bb, body_bb);

   vcode_select_block(exit_bb);

   vcode_dim_t dims[1] = {
      {
         .left  = left_reg,
         .right = right_reg,
         .dir   = dir_reg
      }
   };
   emit_return(emit_wrap(mem_reg, dims, 1));
}

static void lower_predef_reduction_op(tree_t decl, vcode_unit_t context,
                                      subprogram_kind_t kind)
{
   vcode_reg_t r0 = 1;
   type_t r0_type = tree_type(tree_port(decl, 0));

   vcode_type_t vbool = vtype_bool();
   vcode_type_t voffset = vtype_offset();

   vcode_var_t result_var = emit_var(vbool, vbool, ident_uniq("result"), 0);
   vcode_reg_t init_reg =
      emit_const(vbool, kind == S_REDUCE_NAND || kind == S_REDUCE_AND);
   emit_store(init_reg, result_var);

   vcode_var_t i_var = emit_var(voffset, voffset, ident_uniq("i"), 0);
   emit_store(emit_const(vtype_offset(), 0), i_var);

   vcode_reg_t len_reg   = lower_array_len(r0_type, 0, r0);
   vcode_reg_t data_reg  = lower_array_data(r0);
   vcode_reg_t left_reg  = lower_array_left(r0_type, 0, r0);
   vcode_reg_t right_reg = lower_array_right(r0_type, 0, r0);
   vcode_reg_t dir_reg   = lower_array_dir(r0_type, 0, r0);
   vcode_reg_t null_reg  = emit_range_null(left_reg, right_reg, dir_reg);

   vcode_block_t body_bb = emit_block();
   vcode_block_t exit_bb = emit_block();

   emit_cond(null_reg, exit_bb, body_bb);

   vcode_select_block(body_bb);

   vcode_reg_t i_reg   = emit_load(i_var);
   vcode_reg_t src_reg = emit_load_indirect(emit_add(data_reg, i_reg));
   vcode_reg_t cur_reg = emit_load(result_var);

   vcode_reg_t result_reg = VCODE_INVALID_REG;
   switch (kind) {
   case S_REDUCE_OR:
   case S_REDUCE_NOR:
      result_reg = emit_or(cur_reg, src_reg);
      break;
   case S_REDUCE_AND:
   case S_REDUCE_NAND:
      result_reg = emit_and(cur_reg, src_reg);
      break;
   case S_REDUCE_XOR:
   case S_REDUCE_XNOR:
      result_reg = emit_xor(cur_reg, src_reg);
      break;
   default:
      break;
   }

   emit_store(result_reg, result_var);

   vcode_reg_t next_reg = emit_add(i_reg, emit_const(vtype_offset(), 1));
   vcode_reg_t cmp_reg  = emit_cmp(VCODE_CMP_EQ, next_reg, len_reg);
   emit_store(next_reg, i_var);
   emit_cond(cmp_reg, exit_bb, body_bb);

   vcode_select_block(exit_bb);

   if (kind == S_REDUCE_NOR || kind == S_REDUCE_NAND || kind == S_REDUCE_XNOR)
      emit_return(emit_not(emit_load(result_var)));
   else
      emit_return(emit_load(result_var));
}

static void lower_predef_match_op(tree_t decl, vcode_unit_t context,
                                  subprogram_kind_t kind)
{
   vcode_reg_t r0 = 1, r1 = 2;

   type_t r0_type = tree_type(tree_port(decl, 0));
   type_t r1_type = tree_type(tree_port(decl, 1));

   vcode_cmp_t cmp;
   bool invert = false;
   switch (kind) {
   case S_MATCH_NEQ:
      invert = true;
   case S_MATCH_EQ:
      cmp = VCODE_CMP_EQ;
      break;
   case S_MATCH_GE:
      invert = true;
   case S_MATCH_LT:
      cmp = VCODE_CMP_LT;
      break;
   case S_MATCH_GT:
      invert = true;
   case S_MATCH_LE:
      cmp = VCODE_CMP_LEQ;
      break;
   default:
      fatal_trace("invalid match operator %d", kind);
   }

   bool is_array = false, is_bit = false;
   if (type_is_array(r0_type)) {
      is_array = true;
      is_bit = type_ident(type_elem(r0_type)) == std_bit_i;
   }
   else
      is_bit = type_ident(r0_type) == std_bit_i;

   vcode_reg_t result = VCODE_INVALID_REG;
   if (is_array) {
      assert(kind == S_MATCH_EQ || kind == S_MATCH_NEQ);

      vcode_reg_t len0_reg = lower_array_len(r0_type, 0, r0);
      vcode_reg_t len1_reg = lower_array_len(r1_type, 0, r1);

      vcode_block_t fail_bb = emit_block();
      vcode_block_t cont_bb = emit_block();

      vcode_reg_t len_eq = emit_cmp(VCODE_CMP_EQ, len0_reg, len1_reg);
      emit_cond(len_eq, cont_bb, fail_bb);

      vcode_select_block(fail_bb);

      vcode_type_t vseverity = vtype_int(0, SEVERITY_FAILURE - 1);
      vcode_reg_t failure_reg = emit_const(vseverity, SEVERITY_FAILURE);

      vcode_reg_t msg_reg =
         lower_wrap_string("arguments have different lengths");
      vcode_reg_t msg_len = emit_uarray_len(msg_reg, 0);

      emit_debug_info(tree_loc(decl));
      emit_report(emit_unwrap(msg_reg), msg_len, failure_reg);
      emit_jump(cont_bb);

      vcode_select_block(cont_bb);

      vcode_type_t vtype = lower_type(type_elem(r0_type));
      vcode_type_t vbounds = lower_bounds(type_elem(r0_type));
      vcode_reg_t mem_reg = emit_alloca(vtype, vbounds, len0_reg);

      vcode_var_t result_var =
         emit_var(vtype, vbounds, ident_uniq("result"), 0);
      emit_store(emit_const(vtype, 0), result_var);

      vcode_type_t voffset = vtype_offset();
      vcode_var_t i_var = emit_var(voffset, voffset, ident_uniq("i"), 0);
      emit_store(emit_const(vtype_offset(), 0), i_var);

      vcode_reg_t left_reg  = lower_array_left(r0_type, 0, r0);
      vcode_reg_t right_reg = lower_array_right(r0_type, 0, r0);
      vcode_reg_t dir_reg   = lower_array_dir(r0_type, 0, r0);
      vcode_reg_t null_reg  = emit_range_null(left_reg, right_reg, dir_reg);

      vcode_reg_t r0_ptr = lower_array_data(r0);
      vcode_reg_t r1_ptr = lower_array_data(r1);

      vcode_block_t body_bb = emit_block();
      vcode_block_t exit_bb = emit_block();

      emit_cond(null_reg, exit_bb, body_bb);

      vcode_select_block(body_bb);

      vcode_reg_t i_reg = emit_load(i_var);

      vcode_reg_t r0_src_reg = emit_load_indirect(emit_add(r0_ptr, i_reg));
      vcode_reg_t r1_src_reg = emit_load_indirect(emit_add(r1_ptr, i_reg));

      vcode_reg_t tmp;
      if (is_bit)
         tmp = emit_cmp(cmp, r0_src_reg, r1_src_reg);
      else {
         ident_t func = ident_new("IEEE.STD_LOGIC_1164.NVC_REL_MATCH_EQ(UU)U");
         vcode_reg_t context_reg = lower_context_for_call(func);
         vcode_reg_t args[] = { context_reg, r0_src_reg, r1_src_reg };
         tmp = emit_fcall(func, vtype, vbounds, VCODE_CC_PREDEF, args, 3);
      }
      emit_store_indirect(tmp, emit_add(mem_reg, i_reg));

      vcode_reg_t next_reg = emit_add(i_reg, emit_const(vtype_offset(), 1));
      vcode_reg_t cmp_reg  = emit_cmp(VCODE_CMP_EQ, next_reg, len0_reg);
      emit_store(next_reg, i_var);
      emit_cond(cmp_reg, exit_bb, body_bb);

      vcode_select_block(exit_bb);

      vcode_dim_t dims[1] = {
         {
            .left  = left_reg,
            .right = right_reg,
            .dir   = dir_reg
         }
      };
      vcode_reg_t wrap_reg = emit_wrap(mem_reg, dims, 1);

      ident_t func = is_bit
         ? ident_new("STD.STANDARD.\"and\"(Q)J")
         : ident_new("IEEE.STD_LOGIC_1164.\"and\"(Y)U");
      vcode_reg_t context_reg = lower_context_for_call(func);
      vcode_reg_t args[] = { context_reg, wrap_reg };
      result = emit_fcall(func, vtype, vbounds, VCODE_CC_PREDEF, args, 2);
   }
   else if (is_bit)
      result = emit_cmp(cmp, r0, r1);
   else {
      vcode_reg_t context_reg =
         emit_link_package(ident_new("IEEE.STD_LOGIC_1164"));
      vcode_reg_t args[3] = { context_reg, r0, r1 };
      ident_t func = NULL;
      switch (cmp) {
      case VCODE_CMP_LT:
         func = ident_new("IEEE.STD_LOGIC_1164.NVC_REL_MATCH_LT(UU)U");
         break;
      case VCODE_CMP_LEQ:
         func = ident_new("IEEE.STD_LOGIC_1164.NVC_REL_MATCH_LEQ(UU)U");
         break;
      case VCODE_CMP_EQ:
         func = ident_new("IEEE.STD_LOGIC_1164.NVC_REL_MATCH_EQ(UU)U");
         break;
      default:
         fatal_trace("unexpected comparison operator %d", cmp);
      }

      vcode_type_t rtype = lower_type(r0_type);
      result = emit_fcall(func, rtype, rtype, VCODE_CC_PREDEF, args, 3);
   }

   if (invert && is_bit)
      emit_return(emit_not(result));
   else if (invert) {
      ident_t func = ident_new("IEEE.STD_LOGIC_1164.\"not\"(U)4UX01");
      vcode_reg_t context_reg = lower_context_for_call(func);
      vcode_reg_t args[2] = { context_reg, result };
      vcode_type_t rtype = vcode_reg_type(result);
      emit_return(emit_fcall(func, rtype, rtype, VCODE_CC_PREDEF, args, 2));
   }
   else
      emit_return(result);
}

static void lower_predef_min_max(tree_t decl, vcode_unit_t context,
                                 vcode_cmp_t cmp)
{
   type_t type = tree_type(tree_port(decl, 0));

   if (type_is_array(type) && tree_ports(decl) == 1) {
      type_t elem = type_elem(type);
      assert(type_is_scalar(elem));

      vcode_reg_t array_reg = 1;
      vcode_type_t voffset = vtype_offset();

      vcode_var_t i_var = emit_var(voffset, voffset, ident_uniq("i"), 0);
      emit_store(emit_const(voffset, 0), i_var);

      vcode_type_t elem_vtype = lower_type(elem);
      vcode_var_t result_var =
         emit_var(elem_vtype, elem_vtype, ident_uniq("result"), 0);

      tree_t elem_r = range_of(elem, 0);
      vcode_reg_t def_reg =
         (cmp == VCODE_CMP_GT && tree_subkind(elem_r) == RANGE_TO)
         || (cmp == VCODE_CMP_LT && tree_subkind(elem_r) == RANGE_DOWNTO)
         ? lower_range_left(elem_r)
         : lower_range_right(elem_r);

      emit_store(def_reg, result_var);

      vcode_reg_t left_reg  = lower_array_left(type, 0, array_reg);
      vcode_reg_t right_reg = lower_array_right(type, 0, array_reg);
      vcode_reg_t len_reg   = lower_array_len(type, 0, array_reg);
      vcode_reg_t kind_reg  = lower_array_dir(type, 0, array_reg);
      vcode_reg_t data_reg  = lower_array_data(array_reg);
      vcode_reg_t null_reg  = emit_range_null(left_reg, right_reg, kind_reg);

      vcode_block_t body_bb = emit_block();
      vcode_block_t exit_bb = emit_block();

      emit_cond(null_reg, exit_bb, body_bb);

      vcode_select_block(body_bb);

      vcode_reg_t i_reg    = emit_load(i_var);
      vcode_reg_t elem_reg = emit_load_indirect(emit_add(data_reg, i_reg));
      vcode_reg_t cur_reg  = emit_load(result_var);
      vcode_reg_t cmp_reg  = emit_cmp(cmp, elem_reg, cur_reg);
      vcode_reg_t next_reg = emit_select(cmp_reg, elem_reg, cur_reg);
      emit_store(next_reg, result_var);

      vcode_reg_t i_next = emit_add(i_reg, emit_const(voffset, 1));
      emit_store(i_next, i_var);

      vcode_reg_t done_reg = emit_cmp(VCODE_CMP_EQ, i_next, len_reg);
      emit_cond(done_reg, exit_bb, body_bb);

      vcode_select_block(exit_bb);
      emit_return(emit_load(result_var));
   }
   else {
      vcode_reg_t context_reg = 0, r0 = 1, r1 = 2;

      vcode_reg_t test_reg;
      if (type_is_scalar(type))
         test_reg = emit_cmp(cmp, r0, r1);
      else {
         const char *op = cmp == VCODE_CMP_GT ? ">" : "<";
         ident_t func = lower_predef_func_name(type, op);
         vcode_reg_t args[] = { context_reg, r0, r1 };
         vcode_type_t vbool = vtype_bool();
         test_reg = emit_fcall(func, vbool, vbool, VCODE_CC_PREDEF, args, 3);
      }

      emit_return(emit_select(test_reg, r0, r1));
   }
}

static void lower_predef_negate(tree_t decl, vcode_unit_t context,
                                const char *op)
{
   type_t type = tree_type(tree_port(decl, 0));
   vcode_type_t vbool = vtype_bool();
   vcode_reg_t args[] = { 0, 1, 2 };
   vcode_reg_t eq_reg = emit_fcall(lower_predef_func_name(type, op),
                                   vbool, vbool, VCODE_CC_PREDEF, args, 3);

   emit_return(emit_not(eq_reg));
}

static void lower_predef(tree_t decl, vcode_unit_t context)
{
   const subprogram_kind_t kind = tree_subkind(decl);
   if (kind == S_USER || kind == S_FOREIGN || is_open_coded_builtin(kind))
      return;

   ident_t name = tree_ident2(decl);
   if (vcode_find_unit(name) != NULL)
      return;

   type_t type = tree_type(decl);

   vcode_select_unit(context);
   ident_t context_id = vcode_unit_name();

   emit_function(name, tree_loc(decl), context);
   vcode_set_result(lower_func_result_type(type_result(type)));

   lower_push_scope(NULL);

   vcode_type_t vcontext = vtype_context(context_id);
   emit_param(vcontext, vcontext, ident_new("context"));
   lower_subprogram_ports(decl, false);

   switch (tree_subkind(decl)) {
   case S_ARRAY_EQ:
      lower_predef_array_cmp(decl, context, VCODE_CMP_EQ);
      break;
   case S_ARRAY_LE:
      lower_predef_array_cmp(decl, context, VCODE_CMP_LEQ);
      break;
   case S_ARRAY_LT:
      lower_predef_array_cmp(decl, context, VCODE_CMP_LT);
      break;
   case S_ARRAY_GE:
      lower_predef_negate(decl, context, "<");
      break;
   case S_ARRAY_GT:
      lower_predef_negate(decl, context, "<=");
      break;
   case S_RECORD_EQ:
      lower_predef_record_eq(decl, context);
      break;
   case S_ARRAY_NEQ:
   case S_RECORD_NEQ:
      lower_predef_negate(decl, context, "=");
      break;
   case S_TO_STRING:
      lower_predef_to_string(decl, context);
      break;
   case S_SLL:
   case S_SRL:
   case S_SLA:
   case S_SRA:
   case S_ROL:
   case S_ROR:
      lower_predef_bit_shift(decl, context, kind);
      break;
   case S_ARRAY_NOT:
   case S_ARRAY_AND:
   case S_ARRAY_OR:
   case S_ARRAY_XOR:
   case S_ARRAY_XNOR:
   case S_ARRAY_NAND:
   case S_ARRAY_NOR:
      lower_predef_bit_vec_op(decl, context, kind);
      break;
   case S_MIXED_AND:
   case S_MIXED_OR:
   case S_MIXED_XOR:
   case S_MIXED_XNOR:
   case S_MIXED_NAND:
   case S_MIXED_NOR:
      lower_predef_mixed_bit_vec_op(decl, context, kind);
      break;
   case S_REDUCE_OR:
   case S_REDUCE_AND:
   case S_REDUCE_NAND:
   case S_REDUCE_NOR:
   case S_REDUCE_XOR:
   case S_REDUCE_XNOR:
      lower_predef_reduction_op(decl, context, kind);
      break;
   case S_MATCH_EQ:
   case S_MATCH_NEQ:
   case S_MATCH_LT:
   case S_MATCH_LE:
   case S_MATCH_GT:
   case S_MATCH_GE:
      lower_predef_match_op(decl, context, kind);
      break;
   case S_MAXIMUM:
      lower_predef_min_max(decl, context, VCODE_CMP_GT);
      break;
   case S_MINIMUM:
      lower_predef_min_max(decl, context, VCODE_CMP_LT);
      break;
   default:
      break;
   }

   lower_finished();
   lower_pop_scope();
}

static void lower_proc_body(tree_t body, vcode_unit_t context)
{
   const bool never_waits = !!(tree_flags(body) & TREE_F_NEVER_WAITS);

   vcode_select_unit(context);

   ident_t name = tree_ident2(body);
   vcode_unit_t vu = vcode_find_unit(name);
   if (vu != NULL)
      return;

   ident_t context_id = vcode_unit_name();

   if (never_waits)
      vu = emit_function(name, tree_loc(body), context);
   else
      vu = emit_procedure(name, tree_loc(body), context);

   lower_push_scope(body);

   vcode_type_t vcontext = vtype_context(context_id);
   emit_param(vcontext, vcontext, ident_new("context"));

   const bool has_subprograms = lower_has_subprograms(body);
   lower_subprogram_ports(body, has_subprograms || !never_waits);

   lower_decls(body, vu);

   const int nstmts = tree_stmts(body);
   for (int i = 0; i < nstmts; i++)
      lower_stmt(tree_stmt(body, i), NULL);

   if (!vcode_block_finished()) {
      lower_cleanup_protected();
      emit_return(VCODE_INVALID_REG);
   }

   lower_finished();
   lower_pop_scope();

   if (vcode_unit_has_undefined())
      vcode_unit_unref(vu);
}

static vcode_unit_t lower_func_body(tree_t body, vcode_unit_t context)
{
   vcode_select_unit(context);

   ident_t name = tree_ident2(body);
   vcode_unit_t vu = vcode_find_unit(name);
   if (vu != NULL)
      return vu;

   ident_t context_id = vcode_unit_name();

   vu = emit_function(name, tree_loc(body), context);
   vcode_set_result(lower_func_result_type(type_result(tree_type(body))));
   emit_debug_info(tree_loc(body));

   vcode_type_t vcontext = vtype_context(context_id);
   emit_param(vcontext, vcontext, ident_new("context"));

   lower_push_scope(body);

   const bool has_subprograms = lower_has_subprograms(body);
   lower_subprogram_ports(body, has_subprograms);

   lower_decls(body, vu);

   const int nstmts = tree_stmts(body);
   for (int i = 0; i < nstmts; i++)
      lower_stmt(tree_stmt(body, i), NULL);

   lower_finished();
   lower_pop_scope();

   return vu;
}

static void lower_process(tree_t proc, vcode_unit_t context)
{
   vcode_select_unit(context);
   ident_t name = ident_prefix(vcode_unit_name(), tree_ident(proc), '.');
   vcode_unit_t vu = emit_process(name, tree_loc(proc), context);
   emit_debug_info(tree_loc(proc));

   // The code generator assumes the first state starts at block number
   // one. Allocate it here in case lowering the declarations generates
   // additional basic blocks.
   vcode_block_t start_bb = emit_block();
   assert(start_bb == 1);

   lower_push_scope(proc);

   lower_decls(proc, vu);

   // If the last statement in the process is a static wait then this
   // process is always sensitive to the same set of signals and we can
   // emit a single _sched_event call in the reset block
   tree_t wait = NULL;
   const int nstmts = tree_stmts(proc);
   if (nstmts > 0
       && tree_kind((wait = tree_stmt(proc, nstmts - 1))) == T_WAIT
       && (tree_flags(wait) & TREE_F_STATIC_WAIT)) {

      const int ntriggers = tree_triggers(wait);
      for (int i = 0; i < ntriggers; i++)
         lower_sched_event(tree_trigger(wait, i), true);
   }

   emit_return(VCODE_INVALID_REG);

   vcode_select_block(start_bb);

   for (int i = 0; i < nstmts; i++)
      lower_stmt(tree_stmt(proc, i), NULL);

   if (!vcode_block_finished())
      emit_jump(start_bb);

   lower_finished();
   lower_pop_scope();
}

static bool lower_is_signal_ref(tree_t expr)
{
   switch (tree_kind(expr)) {
   case T_REF:
      return class_of(tree_ref(expr)) == C_SIGNAL;

   case T_ALIAS:
   case T_ARRAY_SLICE:
   case T_ARRAY_REF:
   case T_RECORD_REF:
   case T_QUALIFIED:
   case T_TYPE_CONV:
      return lower_is_signal_ref(tree_value(expr));

   default:
      return false;
   }
}

static ident_t lower_converter(tree_t expr, type_t atype, type_t rtype,
                               type_t check_type, vcode_type_t *vatype,
                               vcode_type_t *vrtype)
{
   const tree_kind_t kind = tree_kind(expr);
   tree_t fdecl = kind == T_FCALL ? tree_ref(expr) : NULL;
   bool p0_uarray = false, r_uarray = false;

   // Detect some trivial cases and avoid generating a conversion function
   if (kind == T_TYPE_CONV && type_is_array(atype) && type_is_array(rtype)) {
      if (type_eq(type_elem(atype), type_elem(rtype)))
         return NULL;
   }
   else if (kind == T_TYPE_CONV && type_is_enum(atype) && type_is_enum(rtype))
      return NULL;
   else if (kind == T_FCALL) {
      type_t p0_type = tree_type(tree_port(fdecl, 0));
      p0_uarray = type_is_array(p0_type) && !lower_const_bounds(p0_type);
      r_uarray = type_is_array(rtype) && !lower_const_bounds(rtype);

      if (!p0_uarray && !r_uarray) {
         *vatype = lower_type(atype);
         *vrtype = lower_type(rtype);
         return tree_ident2(fdecl);
      }
   }

   LOCAL_TEXT_BUF tb = tb_new();
   tb_printf(tb, "%s.", istr(vcode_unit_name()));
   if (kind == T_TYPE_CONV)
      tb_printf(tb, "convert_%s_%s", type_pp(atype), type_pp(rtype));
   else {
      tree_t p0 = tree_value(tree_param(expr, 0));
      ident_t signame = tree_ident(name_to_ref(p0));
      tb_printf(tb, "wrap_%s.%s", istr(tree_ident2(fdecl)), istr(signame));
   }
   ident_t name = ident_new(tb_get(tb));

   vcode_unit_t vu = vcode_find_unit(name);
   if (vu != NULL)
      return name;

   vcode_state_t state;
   vcode_state_save(&state);

   vcode_type_t vabounds, vrbounds;
   if (kind == T_TYPE_CONV) {
      *vatype = lower_type(atype);
      *vrtype = lower_type(rtype);
      vabounds = lower_bounds(atype);
      vrbounds = lower_bounds(rtype);
   }
   else {
      if (p0_uarray) {
         type_t elem = lower_elem_recur(atype);
         *vatype = vtype_pointer(lower_type(elem));
         vabounds = lower_bounds(elem);
      }
      else {
         *vatype = lower_type(atype);
         vabounds = lower_bounds(atype);
      }

      if (r_uarray) {
         type_t elem = lower_elem_recur(rtype);
         *vrtype = vtype_pointer(lower_type(elem));
         vrbounds = lower_bounds(elem);
      }
      else {
         *vrtype = lower_type(rtype);
         vrbounds = lower_bounds(rtype);
      }
   }

   ident_t context_id = vcode_unit_name();

   vu = emit_function(name, tree_loc(expr), vcode_active_unit());
   vcode_set_result(*vrtype);
   emit_debug_info(tree_loc(expr));

   lower_push_scope(NULL);

   vcode_type_t vcontext = vtype_context(context_id);
   emit_param(vcontext, vcontext, ident_new("context"));

   vcode_reg_t p0 = emit_param(*vatype, vabounds, ident_new("p0"));

   if (kind == T_TYPE_CONV)
      emit_return(lower_conversion(p0, expr, atype, rtype));
   else {
      vcode_reg_t arg_reg = p0;
      if (p0_uarray)
         arg_reg = lower_wrap(atype, p0);

      ident_t func = tree_ident2(fdecl);
      vcode_reg_t context_reg = lower_context_for_call(func);
      vcode_reg_t args[] = { context_reg, arg_reg };
      vcode_reg_t result_reg = emit_fcall(func, lower_type(rtype), vrbounds,
                                          VCODE_CC_VHDL, args, 2);

      if (r_uarray) {
         lower_check_array_sizes(expr, check_type, rtype,
                                 VCODE_INVALID_REG, result_reg);
         result_reg = emit_unwrap(result_reg);
      }

      emit_return(result_reg);
   }

   lower_pop_scope();
   lower_finished();
   vcode_state_restore(&state);

   return name;
}

static void lower_port_map(tree_t block, tree_t map)
{
   vcode_reg_t port_reg = VCODE_INVALID_REG;
   vcode_reg_t inout_reg = VCODE_INVALID_REG;
   tree_t port = NULL;
   type_t name_type = NULL;
   vcode_reg_t out_conv = VCODE_INVALID_REG;
   vcode_reg_t in_conv = VCODE_INVALID_REG;
   tree_t value = tree_value(map);

   tree_t value_conv = NULL;
   const tree_kind_t value_kind = tree_kind(value);
   if (value_kind == T_FCALL) {
      // See if this is a conversion function
      if (tree_params(value) == 1) {
         tree_t p0 = tree_value(tree_param(value, 0));
         if (lower_is_signal_ref(p0))
            value_conv = p0;
      }
   }
   else if (value_kind == T_TYPE_CONV) {
      tree_t p0 = tree_value(value);
      if (lower_is_signal_ref(p0))
         value_conv = p0;
   }

   switch (tree_subkind(map)) {
   case P_POS:
      {
         port = tree_port(block, tree_pos(map));
         int hops;
         vcode_var_t var = lower_get_var(port, &hops) & 0x3fffffff;
         assert(hops == 0);
         port_reg = emit_load(var);
         name_type = tree_type(port);

         if (tree_subkind(port) == PORT_INOUT) {
            void *key = (void *)((uintptr_t)port | 1);
            var = lower_get_var(key, &hops) & 0x3fffffff;
            assert(hops == 0);
            inout_reg = emit_load(var);
         }
      }
      break;
   case P_NAMED:
      {
         tree_t name = tree_name(map);
         const tree_kind_t kind = tree_kind(name);
         if (kind == T_FCALL) {
            tree_t p0 = tree_value(tree_param(name, 0));
            type_t atype = tree_type(p0);
            type_t rtype = tree_type(name);
            vcode_type_t vatype = VCODE_INVALID_TYPE;
            vcode_type_t vrtype = VCODE_INVALID_TYPE;
            ident_t func = lower_converter(name, atype, rtype,
                                           tree_type(value_conv ?: value),
                                           &vatype, &vrtype);
            vcode_reg_t context_reg = lower_context_for_call(func);
            out_conv = emit_closure(func, context_reg, vatype, vrtype);
            name = p0;
         }
         else if (kind == T_TYPE_CONV) {
            tree_t value = tree_value(name);
            type_t rtype = tree_type(name);
            type_t atype = tree_type(value);
            vcode_type_t vatype = VCODE_INVALID_TYPE;
            vcode_type_t vrtype = VCODE_INVALID_TYPE;
            ident_t func = lower_converter(name, atype, rtype,
                                           tree_type(value_conv ?: value),
                                           &vatype, &vrtype);
            if (func != NULL) {
               vcode_reg_t context_reg = lower_context_for_call(func);
               out_conv = emit_closure(func, context_reg, vatype, vrtype);
            }
            name = value;
         }

         port_reg = lower_expr(name, EXPR_LVALUE);
         port = tree_ref(name_to_ref(name));
         name_type = tree_type(name);

         if (tree_subkind(port) == PORT_INOUT)
            inout_reg = lower_expr(name, EXPR_INPUT_ASPECT);
      }
      break;
   }

   assert(tree_kind(port) == T_PORT_DECL);

   if (vcode_reg_kind(port_reg) == VCODE_TYPE_UARRAY)
      port_reg = lower_array_data(port_reg);

   if (value_kind == T_OPEN)
      value = tree_value(port);
   else if (value_conv != NULL) {
      // Value has conversion function
      type_t atype = tree_type(value_conv);
      type_t rtype = tree_type(value);
      vcode_type_t vatype = VCODE_INVALID_TYPE;
      vcode_type_t vrtype = VCODE_INVALID_TYPE;
      ident_t func = NULL;

      switch (value_kind) {
      case T_FCALL:
         func = lower_converter(value, atype, rtype, name_type,
                                &vatype, &vrtype);
         break;
      case T_TYPE_CONV:
         func = lower_converter(value, atype, rtype, name_type,
                                &vatype, &vrtype);
         break;
      default:
         assert(false);
      }

      if (func != NULL) {
         vcode_reg_t context_reg = lower_context_for_call(func);
         in_conv = emit_closure(func, context_reg, vatype, vrtype);
      }
      value = value_conv;
   }

   if (lower_is_signal_ref(value)) {
      type_t value_type = tree_type(value);
      vcode_reg_t value_reg = lower_expr(value, EXPR_LVALUE);
      const bool input = tree_subkind(port) == PORT_IN;

      vcode_reg_t src_reg = input ? value_reg : port_reg;
      vcode_reg_t dst_reg = input ? port_reg : value_reg;
      vcode_reg_t conv_func = input ? in_conv : out_conv;

      type_t src_type = input ? value_type : name_type;
      type_t dst_type = input ? name_type : value_type;

      vcode_reg_t src_count;
      if (type_is_array(src_type))
         src_count = lower_scalar_sub_elements(src_type, src_reg);
      else
         src_count = emit_const(vtype_offset(), type_width(src_type));

      vcode_reg_t dst_count;
      if (type_is_array(dst_type))
         dst_count = lower_scalar_sub_elements(dst_type, dst_reg);
      else
         dst_count = emit_const(vtype_offset(), type_width(dst_type));

      if (vcode_reg_kind(src_reg) == VCODE_TYPE_UARRAY)
         src_reg = lower_array_data(src_reg);

      if (vcode_reg_kind(dst_reg) == VCODE_TYPE_UARRAY)
         dst_reg = lower_array_data(dst_reg);

      emit_map_signal(src_reg, dst_reg, src_count, dst_count, conv_func);

      // If this is an inout port create the mapping between input and output
      if (inout_reg != VCODE_INVALID_REG)
         emit_map_signal(value_reg, inout_reg, dst_count, src_count, in_conv);
   }
   else {
      vcode_reg_t value_reg = lower_expr(value, EXPR_RVALUE);
      lower_sub_signals(name_type, port, port_reg, value_reg,
                        VCODE_INVALID_REG);

      if (inout_reg != VCODE_INVALID_REG) {
         vcode_reg_t count_reg;
         if (type_is_array(name_type))
            count_reg = lower_scalar_sub_elements(name_type, port_reg);
         else
            count_reg = emit_const(vtype_offset(), type_width(name_type));

         emit_map_signal(port_reg, inout_reg, count_reg, count_reg, in_conv);
      }
   }
}

static void lower_port_decl(tree_t port, ident_t suffix)
{
   ident_t pname = ident_prefix(tree_ident(port), suffix, '$');
   type_t type = tree_type(port);

   vcode_type_t vtype = lower_signal_type(type);
   vcode_var_t var = emit_var(vtype, vtype, pname, VAR_SIGNAL);
   vcode_reg_t shared = VCODE_INVALID_REG;

   if (vtype_kind(vtype) == VCODE_TYPE_UARRAY) {
      shared = emit_link_signal(pname, vtype_elem(vtype));
      emit_store(lower_wrap(type, shared), var);
   }
   else {
      shared = emit_link_signal(pname, vtype);
      emit_store(shared, var);
   }

   void *key = suffix ? (void *)((uintptr_t)port | 1) : port;
   lower_put_vcode_obj(key, var | 0x80000000, top_scope);

   vcode_reg_t init_reg;
   if (tree_has_value(port))
      init_reg = lower_expr(tree_value(port), EXPR_RVALUE);
   else
      init_reg = lower_default_value(type, false);

   lower_sub_signals(type, port, shared, init_reg, VCODE_INVALID_REG);
}

static void lower_ports(tree_t block)
{
   const int nports = tree_ports(block);
   for (int i = 0; i < nports; i++) {
      tree_t p = tree_port(block, i);
      if (tree_subkind(p) == PORT_INOUT) {
         lower_port_decl(p, NULL);
         lower_port_decl(p, ident_new("in"));
      }
      else
         lower_port_decl(p, NULL);
   }

   const int nparams = tree_params(block);
   for (int i = 0; i < nparams; i++)
      lower_port_map(block, tree_param(block, i));
}

static void lower_generics(tree_t block)
{
   const int ngenerics = tree_generics(block);
   assert(ngenerics == tree_genmaps(block));

   for (int i = 0; i < ngenerics; i++) {
      tree_t g = tree_generic(block, i);
      tree_t m = tree_genmap(block, i);
      assert(tree_subkind(m) == P_POS);

      type_t type = tree_type(g);

      vcode_type_t vtype = lower_type(type);
      vcode_type_t vbounds = lower_bounds(type);
      vcode_var_t var = emit_var(vtype, vbounds, tree_ident(g), VAR_CONST);

      vcode_reg_t mem_reg = VCODE_INVALID_REG, count_reg = VCODE_INVALID_REG;
      uint32_t hint = VCODE_INVALID_HINT;

      const bool is_array = type_is_array(type);

      if (is_array && lower_const_bounds(type)) {
         mem_reg = emit_index(var, VCODE_INVALID_REG);
         count_reg = lower_array_total_len(type, VCODE_INVALID_REG);
         hint = emit_storage_hint(mem_reg, count_reg);
      }
      else if (type_is_record(type))
         mem_reg = emit_index(var, VCODE_INVALID_REG);

      tree_t value = tree_value(m);
      vcode_reg_t value_reg = lower_expr(value, EXPR_RVALUE);

      if (is_array && mem_reg != VCODE_INVALID_REG)
         lower_check_array_sizes(g, type, tree_type(value),
                                 VCODE_INVALID_REG, value_reg);
      else if (type_is_scalar(type)) {
         value_reg = lower_reify(value_reg);
         lower_check_scalar_bounds(value_reg, type, g, value);
      }

      if (mem_reg != VCODE_INVALID_REG)
         emit_copy(mem_reg, lower_array_data(value_reg), count_reg);
      else if (is_array)
         emit_store(lower_wrap(tree_type(value), value_reg), var);
      else
         emit_store(value_reg, var);

      if (hint != VCODE_INVALID_HINT)
         vcode_clear_storage_hint(hint);

      lower_put_vcode_obj(g, var | 0x40000000, top_scope);
   }
}

static vcode_unit_t lower_concurrent_block(tree_t block, vcode_unit_t context)
{
   vcode_select_unit(context);

   ident_t prefix = context ? vcode_unit_name() : lib_name(lib_work());
   ident_t name = ident_prefix(prefix, tree_ident(block), '.');

   const loc_t *loc = tree_loc(block);
   vcode_unit_t vu = emit_instance(name, loc, context);
   emit_debug_info(loc);

   lower_push_scope(block);
   lower_generics(block);
   lower_ports(block);
   lower_decls(block, vu);

   emit_return(VCODE_INVALID_REG);
   lower_finished();

   const int nstmts = tree_stmts(block);
   for (int i = 0; i < nstmts; i++) {
      tree_t s = tree_stmt(block, i);
      switch (tree_kind(s)) {
      case T_BLOCK:
         lower_concurrent_block(s, vu);
         break;
      case T_PROCESS:
         lower_process(s, vu);
         break;
      default:
         fatal_trace("cannot handle tree kind %s in lower_concurrent_block",
                     tree_kind_str(tree_kind(s)));
      }
   }

   lower_pop_scope();
   return vu;
}

static vcode_unit_t lower_elab(tree_t unit)
{
   assert(tree_decls(unit) == 0);
   assert(tree_stmts(unit) == 1);

   tree_t top = tree_stmt(unit, 0);
   assert(tree_kind(top) == T_BLOCK);
   return lower_concurrent_block(top, NULL);
}

static vcode_unit_t lower_pack_body(tree_t unit)
{
   tree_t pack = tree_primary(unit);

   vcode_unit_t context = emit_package(tree_ident(pack), tree_loc(unit));
   lower_push_scope(unit);
   top_scope->flags |= SCOPE_GLOBAL;

   lower_decls(pack, context);
   lower_decls(unit, context);

   emit_return(VCODE_INVALID_REG);

   lower_finished();
   lower_pop_scope();
   return context;
}

static vcode_unit_t lower_package(tree_t unit)
{
   vcode_unit_t context = emit_package(tree_ident(unit), tree_loc(unit));
   lower_push_scope(unit);
   top_scope->flags |= SCOPE_GLOBAL;

   lower_decls(unit, context);

   emit_return(VCODE_INVALID_REG);

   lower_finished();
   lower_pop_scope();
   return context;
}

static void lower_set_verbose(void)
{
   static bool set = false;
   if (!set) {
      const char *venv = getenv("NVC_LOWER_VERBOSE");
      if (venv != NULL && venv[0] != '\0')
         verbose = isalpha((int)venv[0]) || venv[0] == ':' ? venv : "";
      else
         verbose = opt_get_str("dump-vcode");
   }
}

vcode_unit_t lower_unit(tree_t unit, cover_tagging_t *cover)
{
   assert(top_scope == NULL);
   lower_set_verbose();

   cover_tags = cover;
   mode = LOWER_NORMAL;

   vcode_unit_t root = NULL;
   switch (tree_kind(unit)) {
   case T_ELAB:
      root = lower_elab(unit);
      break;
   case T_PACK_BODY:
      root = lower_pack_body(unit);
      break;
   case T_PACKAGE:
      assert(!package_needs_body(unit));
      root = lower_package(unit);
      break;
   default:
      fatal("cannot lower unit kind %s to vcode",
            tree_kind_str(tree_kind(unit)));
   }

   vcode_close();
   return root;
}

vcode_unit_t lower_thunk(tree_t t)
{
   assert(top_scope == NULL);
   lower_set_verbose();

   mode = LOWER_THUNK;

   const tree_kind_t kind = tree_kind(t);

   ident_t name = NULL;
   if (kind == T_FUNC_BODY) {
      name = ident_prefix(tree_ident2(t), thunk_i, '$');

      vcode_unit_t vu = vcode_find_unit(name);
      if (vu != NULL)
         return vu;
   }

   vcode_unit_t thunk = emit_thunk(name);

   if (kind == T_FUNC_BODY) {
      vcode_set_result(lower_func_result_type(type_result(tree_type(t))));
      emit_debug_info(tree_loc(t));

      vcode_type_t vcontext = vtype_context(ident_new("dummy"));
      emit_param(vcontext, vcontext, ident_new("context"));

      lower_push_scope(t);

      const bool has_subprograms = lower_has_subprograms(t);
      if (has_subprograms) {
         // This doesn't work yet as the name of nested subprograms
         // won't be mangled with $thunk
         lower_pop_scope();
         vcode_unit_unref(thunk);
         return NULL;
      }

      lower_subprogram_ports(t, has_subprograms);

      lower_decls(t, thunk);

      const int nstmts = tree_stmts(t);
      for (int i = 0; i < nstmts; i++)
         lower_stmt(tree_stmt(t, i), NULL);

      lower_pop_scope();
   }
   else {
      vcode_type_t vtype = VCODE_INVALID_TYPE;
      if (tree_kind(t) == T_FCALL) {
         tree_t decl = tree_ref(t);
         if (tree_has_type(decl))
            vtype = lower_func_result_type(type_result(tree_type(decl)));
      }

      if (vtype == VCODE_INVALID_TYPE)
         vtype = lower_type(tree_type(t));

      vcode_set_result(vtype);

      vcode_reg_t result_reg = lower_expr(t, EXPR_RVALUE);
      if (type_is_scalar(tree_type(t)))
         emit_return(emit_cast(vtype, vtype, result_reg));
      else
         emit_return(result_reg);
   }

   lower_finished();

   if (vcode_unit_has_undefined()) {
      vcode_unit_unref(thunk);
      return NULL;
   }

   vcode_close();
   return thunk;
}
