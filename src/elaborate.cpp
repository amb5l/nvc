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
#include "phase.h"
#include "lib.h"
#include "common.h"

#include <cassert>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <cinttypes>

class Elaborator {
public:
   Elaborator(TopLevel *top);
   Elaborator(const Elaborator &) = delete;

   void elaborate(tree_t unit);

private:
   struct Context {
      lib_t library;
      ident_t path;
      ident_t inst;
      tree_t arch;
   };

   tree_t pick_arch(const loc_t *loc, ident_t name, lib_t *new_lib,
                    const Context &context);
   lib_t find_lib(ident_t name, const Context &context);
   void elab_entity_arch(tree_t entity, tree_t arch, const Context &context);
   void elab_instance(tree_t inst, const Context &context);
   void elab_arch(tree_t arch, const Context &context);
   void elab_stmts(tree_t unit, const Context &context);
   void elab_decls(tree_t tree, const Context& context);
   void elab_signal(tree_t decl, const Context& context);
   void elab_port_map(tree_t instance, tree_t entity);
   void push_scope(tree_t unit, const Context &context);
   void pop_scope();
   tree_t elab_copy(tree_t t);

   static ident_t hpathf(ident_t path, char sep, const char *fmt, ...);
   static const char *simple_name(const char *full);

   TopLevel *top_;
   Scope    *scope_  = nullptr;
   netid_t   next_nid_ = 0;
};

typedef struct {
   lib_t lib;
   ident_t name;
   tree_t *tree;
} lib_search_params_t;

typedef struct copy_list copy_list_t;

struct copy_list {
   copy_list_t *next;
   tree_t tree;
};

static void elab_hint_fn(void *arg)
{
   tree_t t = (tree_t)arg;

   LOCAL_TEXT_BUF tb = tb_new();
   tb_printf(tb, "while elaborating instance %s", istr(tree_ident(t)));

   const int ngenerics = tree_genmaps(t);
   for (int i = 0; i < ngenerics; i++) {
      tree_t p = tree_genmap(t, i);
      ident_t name = NULL;
      switch (tree_subkind(p)) {
      case P_POS:
         name = tree_ident(tree_generic(tree_ref(t), tree_pos(p)));
         break;
      case P_NAMED:
         name = tree_ident(tree_name(p));
         break;
      default:
         continue;
      }

      tb_printf(tb, "\n\t%s => ", istr(name));

      tree_t value = tree_value(p);
      switch (tree_kind(value)) {
      case T_LITERAL:
         switch (tree_subkind(value)) {
         case L_INT: tb_printf(tb, "%" PRIi64, tree_ival(value)); break;
         case L_REAL: tb_printf(tb, "%lf", tree_dval(value)); break;
         }
         break;
      default:
         tb_printf(tb, "...");
      }
   }

   note_at(tree_loc(t), "%s", tb_get(tb));
}

static bool elab_should_copy(tree_t t)
{
   switch (tree_kind(t)) {
   case T_SIGNAL_DECL:
   case T_GENVAR:
   case T_PROCESS:
   case T_ARCH:
      return true;
   case T_LITERAL:
   case T_ASSOC:
   case T_PARAM:
   case T_WAVEFORM:
   case T_ARRAY_SLICE:
   case T_UNIT_DECL:
   case T_USE:
   case T_IF_GENERATE:
   case T_CONCAT:
   case T_LIBRARY:
   case T_TYPE_CONV:
   case T_ALL:
   case T_OPEN:
   case T_ATTR_REF:
   case T_NEW:
   case T_BINDING:
   case T_SPEC:
   case T_AGGREGATE:
   case T_CONSTRAINT:
   case T_QUALIFIED:
       return false;
   case T_VAR_DECL:
      if (tree_flags(t) & TREE_F_SHARED)
         return true;
      // Fall-through
   default:
      return tree_attr_int(t, elab_copy_i, 0);
   }
}

static bool elab_copy_trees(tree_t t, void *context)
{
   copy_list_t *list = (copy_list_t *)context;

   if (elab_should_copy(t)) {
      for (; list != NULL; list = list->next) {
         if (list->tree == t)
            return true;
      }
   }

   return false;
}

static void elab_build_copy_list(tree_t t, void *context)
{
   copy_list_t **list = (copy_list_t **)context;

   if (elab_should_copy(t)) {
      copy_list_t *cnew = (copy_list_t *)xmalloc(sizeof(copy_list_t));
      cnew->tree = t;
      cnew->next = *list;

      *list = cnew;
   }
}

static void find_arch(ident_t name, int kind, void *context)
{
   lib_search_params_t *params = (lib_search_params_t *)context;

   ident_t prefix = ident_until(name, '-');

   if ((kind == T_ARCH) && (prefix == params->name)) {
      tree_t t = lib_get_check_stale(params->lib, name);
      assert(t != NULL);

      if (*(params->tree) == NULL)
         *(params->tree) = t;
      else {
         lib_mtime_t old_mtime = lib_mtime(params->lib,
                                           tree_ident(*(params->tree)));
         lib_mtime_t new_mtime = lib_mtime(params->lib, tree_ident(t));

         if (new_mtime == old_mtime) {
            // Analysed at the same time: compare line number
            // Note this assumes both architectures are from the same
            // file but this shouldn't be a problem with high-resolution
            // timestamps
            uint16_t new_line = tree_loc(t)->first_line;
            uint16_t old_line = tree_loc(*(params->tree))->first_line;

            if (new_line > old_line)
               *(params->tree) = t;
         }
         else if (new_mtime > old_mtime)
            *(params->tree) = t;
      }
   }
}

Elaborator::Elaborator(TopLevel *top)
   : top_(top)
{

}

const char *Elaborator::simple_name(const char *full)
{
   // Strip off any library or entity prefix from the parameter
   const char *start = full;
   for (const char *p = full; *p != '\0'; p++) {
      if (*p == '.' || *p == '-')
         start = p + 1;
   }

   return start;
}

ident_t Elaborator::hpathf(ident_t path, char sep, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   char *buf = xvasprintf(fmt, ap);
   va_end(ap);

   // LRM specifies instance path is lowercase
   char *p = buf;
   while (*p != '\0') {
      *p = tolower((int)*p);
      ++p;
   }

   ident_t id = ident_new(buf);
   free(buf);
   return ident_prefix(path, id, sep);
}

lib_t Elaborator::find_lib(ident_t name, const Context& context)
{
   ident_t lib_name = ident_until(name, '.');
   if (lib_name == work_i)
      return context.library;
   else
      return lib_find(lib_name, true);
}

tree_t Elaborator::pick_arch(const loc_t *loc, ident_t name, lib_t *new_lib,
                             const Context& context)
{
   // When an explicit architecture name is not given select the most
   // recently analysed architecture of this entity

   lib_t lib = find_lib(name, context);
   ident_t search_name =
      ident_prefix(lib_name(lib), ident_rfrom(name, '.'), '.');

   tree_t arch = lib_get_check_stale(lib, search_name);
   if ((arch == NULL) || (tree_kind(arch) != T_ARCH)) {
      arch = NULL;
      lib_search_params_t params = { lib, search_name, &arch };
      lib_walk_index(lib, find_arch, &params);

      if (arch == NULL)
         fatal_at(loc, "no suitable architecture for %s", istr(search_name));
   }

   if (new_lib != NULL)
      *new_lib = lib;

   return arch;
}

void Elaborator::push_scope(tree_t unit, const Context& context)
{
   if (tree_kind(unit) == T_PACKAGE) {
      const char *name = istr(tree_ident(unit));
      char lower[strlen(name) + 1], *p;
      for (p = lower; *name != '\0'; p++, name++)
         *p = tolower((int)*name);
      *p = '\0';

      scope_ = new Scope(scope_, ident_new(lower));
   }
   else
      scope_ = new Scope(scope_,
                         ident_new(strrchr(istr(context.path), ':') + 1));

   if (top_->root() == nullptr)
      top_->root_ = scope_;

#if 0
   if ((tree_kind(unit) == T_ARCH) && (tree_decls(ctx->out) > 0)) {
      // Convert an identifier like WORK.FOO-RTL to foo(rtl)
      const char *id = istr(tree_ident(unit));
      while ((*id != '\0') && (*id++ != '.'))
         ;

      const size_t len = strlen(id);
      char str[len + 2];
      char *p = str;
      for (; *id != '\0'; p++, id++) {
         if (*id == '-')
            *p = '(';
         else
            *p = tolower((int)*id);
      }

      *p++ = ')';
      *p++ = '\0';

      tree_set_ident2(h, ident_new(str));
   }
#endif
}

void Elaborator::pop_scope()
{
   scope_ = scope_->parent();
}

tree_t Elaborator::elab_copy(tree_t t)
{
   copy_list_t *copy_list = NULL;
   tree_visit(t, elab_build_copy_list, &copy_list);

   // For achitectures, also make a copy of the entity ports
   if (tree_kind(t) == T_ARCH)
      tree_visit(tree_ref(t), elab_build_copy_list, &copy_list);

   tree_t copy = tree_copy(t, elab_copy_trees, copy_list);

   while (copy_list != NULL) {
      copy_list_t *tmp = copy_list->next;
      free(copy_list);
      copy_list = tmp;
   }

   return copy;
}

void Elaborator::elab_port_map(tree_t instance, tree_t entity)
{

}

void Elaborator::elab_instance(tree_t inst, const Context& context)
{
   lib_t new_lib = NULL;
   tree_t arch = NULL;
   switch (tree_class(inst)) {
   case C_ENTITY:
      arch = elab_copy(pick_arch(tree_loc(inst), tree_ident2(inst), &new_lib,
                                 context));
      break;

   case C_COMPONENT:
      //arch = elab_default_binding(t, &new_lib, ctx);
      break;

   case C_CONFIGURATION:
      fatal_at(tree_loc(inst), "sorry, configurations is not supported yet");
      break;

   default:
      assert(false);
   }

   if (arch == NULL)
      return;

#if 0
   map_list_t *maps = elab_map(t, arch, tree_ports, tree_port,
                               tree_params, tree_param);

   (void)elab_map(t, arch, tree_generics, tree_generic,
                  tree_genmaps, tree_genmap);
#endif

   ident_t ninst = hpathf(context.inst, '@', "%s(%s)",
                          simple_name(istr(tree_ident2(arch))),
                          simple_name(istr(tree_ident(arch))));

   Context new_ctx = {
      .library  = new_lib,
      .path     = context.path,
      .inst     = ninst,
      .arch     = arch
   };

   tree_t entity = tree_ref(arch);

   push_scope(entity, context);

   //elab_copy_context(entity, &new_ctx);
   //elab_decls(entity, &new_ctx);

   //elab_map_nets(maps);
   //elab_free_maps(maps);

   set_hint_fn(elab_hint_fn, inst);
   simplify(arch, EVAL_LOWER);
   bounds_check(arch);
   clear_hint();

   if (eval_errors() == 0 && bounds_errors() == 0) {
      elab_arch(arch, new_ctx);
   }

   pop_scope();
}

void Elaborator::elab_stmts(tree_t unit, const Context& context)
{
   const int nstmts = tree_stmts(unit);
   for (int i = 0; i < nstmts; i++) {
      tree_t s = tree_stmt(unit, i);
      const char *label = istr(tree_ident(s));
      ident_t npath = hpathf(context.path, ':', "%s", label);
      ident_t ninst = hpathf(context.inst, ':', "%s", label);

      Context new_ctx = {
         .library  = context.library,
         .path     = npath,
         .inst     = ninst,
         .arch     = context.arch
      };

      switch (tree_kind(s)) {
      case T_INSTANCE:
         elab_instance(s, new_ctx);
         break;
      case T_BLOCK:
         //elab_block(s, &new_ctx);
         break;
      case T_FOR_GENERATE:
         //elab_for_generate(s, &new_ctx);
         break;
      case T_IF_GENERATE:
         //elab_if_generate(s, &new_ctx);
         break;
      case T_PROCESS:
         //elab_process(s, &new_ctx);
         break;
      default:
         break;
      }

      //tree_set_ident(s, npath);
   }
}

void Elaborator::elab_signal(tree_t decl, const Context& context)
{
   Signal *s = new Signal(tree_ident(decl));
   scope_->signals_.add(s);

   Net *n = new Net(next_nid_++, type_width(tree_type(decl)), 1 /* XXX */);
   s->nets_.add(n);
}

void Elaborator::elab_decls(tree_t tree, const Context& context)
{
   //tree_add_attr_str(t, inst_name_i,
   //                  ident_prefix(ctx->inst, ident_new(":"), '\0'));

   const int ndecls = tree_decls(tree);
   for (int i = 0; i < ndecls; i++) {
      tree_t d = tree_decl(tree, i);
      const char *label = simple_name(istr(tree_ident(d)));
      ident_t ninst = hpathf(context.inst, ':', "%s", label);
      ident_t npath = hpathf(context.path, ':', "%s", label);

      if (label[0] == ':')
         continue;  // Already named one instance of this

      switch (tree_kind(d)) {
      case T_SIGNAL_DECL:
         elab_signal(d, context);
         break;
      case T_FUNC_BODY:
      case T_PROC_BODY:
      case T_ALIAS:
      case T_FILE_DECL:
      case T_VAR_DECL:
         //tree_set_ident(d, npath);
         //tree_add_decl(context.out, d);
         //tree_add_attr_str(d, inst_name_i, ninst);
         break;
      case T_PROT_BODY:
         //tree_set_ident(d, npath);
         //elab_prot_body_decls(d);
         //tree_add_decl(context.out, d);
         break;
      case T_FUNC_DECL:
      case T_PROC_DECL:
         //elab_set_subprogram_name(d, npath);
         break;
      case T_CONST_DECL:
         //tree_set_ident(d, npath);
         //tree_add_attr_str(d, inst_name_i, ninst);
         //tree_add_decl(context.out, d);
         break;
      case T_USE:
         //elab_use_clause(d, ctx);
         break;
      default:
         break;
      }
   }
}

void Elaborator::elab_arch(tree_t arch, const Context& context)
{
   //elab_stmts(tree_ref(t), ctx);
   //elab_pseudo_context(context.out, t);
   //elab_pseudo_context(ctx->out, tree_ref(t));
   //elab_copy_context(t, ctx);
   elab_decls(arch, context);
   elab_stmts(arch, context);

   //tree_rewrite(t, fixup_entity_refs, t);

   //   tree_set_ident(t, ident_prefix(ctx->path, ident_new(":"), '\0'));
}


void Elaborator::elab_entity_arch(tree_t entity, tree_t arch,
                                  const Context& context)
{
   const char *name = simple_name(istr(tree_ident(entity)));
   ident_t ninst = hpathf(context.inst, ':', ":%s(%s)", name,
                          simple_name(istr(tree_ident(arch))));
   ident_t npath = hpathf(context.path, ':', ":%s", name);

   Context new_context = {
      .library  = context.library,
      .path     = npath,
      .inst     = ninst,
      .arch     = arch
   };

   push_scope(entity, new_context);

   //elab_top_level_ports(arch, context);
   //elab_top_level_generics(arch, context);

   //elab_pseudo_context(context->out, t);
   //elab_copy_context(t, context);
   //elab_decls(t, context);

   //tree_add_attr_str(context->out, simple_name_i, npath);

   simplify(arch, EVAL_LOWER);
   bounds_check(arch);

   if (bounds_errors() == 0 && eval_errors() == 0) {
      elab_arch(arch, new_context);
   }

   pop_scope();
}

void Elaborator::elaborate(tree_t unit)
{
   Context context = {
      .library = lib_work(),
   };

   switch (tree_kind(unit)) {
   case T_ENTITY:
      {
         tree_t arch = pick_arch(NULL, tree_ident(unit), NULL, context);
         elab_entity_arch(unit, arch, context);
      }
      break;
   case T_ARCH:
      elab_entity_arch(tree_ref(unit), unit, context);
      break;
   default:
      fatal("%s is not a suitable top-level unit", istr(tree_ident(unit)));
   }
}

TopLevel *elaborate(tree_t tree)
{
   TopLevel *top = new TopLevel;

   Elaborator elab(top);
   elab.elaborate(tree);

   return top;
}
