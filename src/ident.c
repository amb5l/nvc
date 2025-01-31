//
//  Copyright (C) 2011-2020  Nick Gasson
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
#include "fbuf.h"
#include "ident.h"

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#define MAP_DEPTH 3

typedef struct clist clist_t;
typedef struct trie  trie_t;

struct clist {
   unsigned char  value;
   trie_t        *down;
   clist_t       *left;
   clist_t       *right;
};

struct trie {
   unsigned char  value;
   uint16_t       write_gen;
   uint16_t       depth;
   uint32_t       write_index;
   trie_t        *up;
   clist_t       *list;
   trie_t        *map[0];
};

struct ident_rd_ctx {
   fbuf_t  *file;
   size_t   cache_sz;
   size_t   cache_alloc;
   ident_t *cache;
};

struct ident_wr_ctx {
   fbuf_t        *file;
   uint32_t       next_index;
   uint16_t       generation;
   unsigned char *scratch;
   size_t         scratch_size;
};

typedef struct {
   trie_t  trie;
   trie_t *map[256];
} root_t;

static root_t root = {
   {
      .value       = '\0',
      .write_gen   = 0,
      .write_index = 0,
      .depth       = 1,
      .up          = NULL
   }
};

static trie_t *alloc_node(char ch, trie_t *prev)
{
   const size_t mapsz = (prev->depth < MAP_DEPTH) ? 256 * sizeof(trie_t *) : 0;

   trie_t *t = xmalloc(sizeof(trie_t) + mapsz);
   t->value     = ch;
   t->depth     = prev->depth + 1;
   t->up        = prev;
   t->write_gen = 0;
   t->list      = NULL;

   if (mapsz > 0)
      memset(t->map, '\0', mapsz);

   if (prev->depth <= MAP_DEPTH)
      prev->map[(unsigned char)ch] = t;
   else {
      clist_t *c = xmalloc(sizeof(clist_t));
      c->value    = ch;
      c->down     = t;
      c->left     = NULL;
      c->right    = NULL;

      clist_t *it, **where;
      for (it = prev->list, where = &(prev->list);
           it != NULL;
           where = (ch < it->value ? &(it->left) : &(it->right)),
              it = *where)
         ;

      *where = c;
   }

   return t;
}

static void build_trie(const char *str, trie_t *prev, trie_t **end)
{
   assert(*str != '\0');
   assert(prev != NULL);

   trie_t *t = alloc_node(*str, prev);

   if (*(++str) == '\0')
      *end = t;
   else
      build_trie(str, t, end);
}

static clist_t *search_node(trie_t *t, char ch)
{
   clist_t *it;
   for (it = t->list;
        (it != NULL) && (it->value != ch);
        it = (ch < it->value ? it->left : it->right))
      ;

   return it;
}

static bool search_trie(const char **str, trie_t *t, trie_t **end)
{
   assert(**str != '\0');
   assert(t != NULL);

   trie_t *next = NULL;

   if (t->depth <= MAP_DEPTH)
      next = t->map[(unsigned char)**str];
   else {
      clist_t *it = search_node(t, **str);
      next = (it != NULL) ? it->down : NULL;
   }

   if (next == NULL) {
      *end = t;
      return false;
   }
   else {
      (*str)++;

      if (**str == '\0') {
         *end = next;
         return true;
      }
      else
         return search_trie(str, next, end);
   }
}

ident_t ident_new(const char *str)
{
   assert(str != NULL);
   assert(*str != '\0');

   trie_t *result;
   if (!search_trie(&str, &(root.trie), &result))
      build_trie(str, result, &result);

   return result;
}

bool ident_interned(const char *str)
{
   assert(str != NULL);
   assert(*str != '\0');

   trie_t *result;
   return search_trie(&str, &(root.trie), &result);
}

void istr_r(ident_t ident, char *buf, size_t sz)
{
   char *p = buf + ident->depth - 1;
   assert(p < buf + sz);
   *p-- = '\0';

   trie_t *it;
   for (it = ident; it->value != '\0'; it = it->up)
      *(p--) = it->value < 128 ? it->value : '?';
}

const char *istr(ident_t ident)
{
   if (ident == NULL)
      return NULL;

#if 1
   char *p = get_fmt_buf(ident->depth);
   istr_r(ident, p, ident->depth);
#else
   char *p = get_fmt_buf(ident->depth * 5) + ident->depth * 5 - 1;
   *p = '\0';

   trie_t *it;
   for (it = ident; it->value != '\0'; it = it->up) {
      if (it->value < 128)
         *(--p) = it->value;
      else {
         *(--p) = '0' + (it->value & 7);
         *(--p) = '0' + ((it->value >> 3) & 7);
         *(--p) = '0' + ((it->value >> 6) & 7);
         *(--p) = '0';
         *(--p) = '\\';
      }
   }
#endif

   return p;
}

ident_wr_ctx_t ident_write_begin(fbuf_t *f)
{
   static uint16_t ident_wr_gen = 1;
   assert(ident_wr_gen > 0);

   struct ident_wr_ctx *ctx = xcalloc(sizeof(struct ident_wr_ctx));
   ctx->file         = f;
   ctx->generation   = ident_wr_gen++;
   ctx->scratch_size = 100;
   ctx->scratch      = xmalloc(ctx->scratch_size);
   ctx->next_index   = 1;   // Skip over null ident

   return ctx;
}

void ident_write_end(ident_wr_ctx_t ctx)
{
   free(ctx->scratch);
   free(ctx);
}

void ident_write(ident_t ident, ident_wr_ctx_t ctx)
{
   if (ident == NULL)
      fbuf_put_uint(ctx->file, 1);
   else if (ident->write_gen == ctx->generation)
      fbuf_put_uint(ctx->file, ident->write_index + 1);
   else {
      fbuf_put_uint(ctx->file, 0);

      if (ident->depth > ctx->scratch_size) {
         ctx->scratch_size = next_power_of_2(ident->depth);
         ctx->scratch = xrealloc(ctx->scratch, ctx->scratch_size);
      }

      unsigned char *p = ctx->scratch + ident->depth - 1;
      *p = '\0';

      trie_t *it;
      for (it = ident; it->value != '\0'; it = it->up)
         *(--p) = it->value;

      write_raw(ctx->scratch, ident->depth, ctx->file);

      ident->write_gen   = ctx->generation;
      ident->write_index = ctx->next_index++;

      assert(ctx->next_index != UINT32_MAX);
   }
}

ident_rd_ctx_t ident_read_begin(fbuf_t *f)
{
   struct ident_rd_ctx *ctx = xmalloc(sizeof(struct ident_rd_ctx));
   ctx->file        = f;
   ctx->cache_alloc = 256;
   ctx->cache_sz    = 0;
   ctx->cache       = xmalloc_array(ctx->cache_alloc, sizeof(ident_t));

   // First index is implicit null
   ctx->cache[ctx->cache_sz++] = NULL;

   return ctx;
}

void ident_read_end(ident_rd_ctx_t ctx)
{
   free(ctx->cache);
   free(ctx);
}

ident_t ident_read(ident_rd_ctx_t ctx)
{
   const uint32_t index = fbuf_get_uint(ctx->file);
   if (index == 0) {
      if (ctx->cache_sz == ctx->cache_alloc) {
         ctx->cache_alloc *= 2;
         ctx->cache = xrealloc(ctx->cache, ctx->cache_alloc * sizeof(ident_t));
      }

      trie_t *p = &(root.trie);
      char ch;
      while ((ch = read_u8(ctx->file)) != '\0') {
         trie_t *next = NULL;
         if (p->depth <= MAP_DEPTH)
            next = p->map[(unsigned char)ch];
         else {
            clist_t *it = search_node(p, ch);
            next = (it != NULL) ? it->down : NULL;
         }

         if (next != NULL)
            p = next;
         else
            p = alloc_node(ch, p);
      }

      if (p == &(root.trie))
         return NULL;
      else {
         ctx->cache[ctx->cache_sz++] = p;
         return p;
      }
   }
   else if (likely(index - 1 < ctx->cache_sz))
      return ctx->cache[index - 1];
   else
      fatal("ident index in %s is corrupt: index=%d cache_sz=%d",
            fbuf_file_name(ctx->file), index, (int)ctx->cache_sz);
}

ident_t ident_uniq(const char *prefix)
{
   static int counter = 0;

   const char *start = prefix;
   trie_t *end;
   if (search_trie(&start, &(root.trie), &end)) {
      const size_t len = strlen(prefix) + 16;
      char buf[len];
      snprintf(buf, len, "%s%d", prefix, counter++);

      return ident_new(buf);
   }
   else {
      trie_t *result;
      build_trie(start, end, &result);
      return result;
   }
}

ident_t ident_prefix(ident_t a, ident_t b, char sep)
{
   if (a == NULL)
      return b;
   else if (b == NULL)
      return a;

   trie_t *result;

   if (sep != '\0') {
      // Append separator
      const char sep_str[] = { sep, '\0' };
      const char *p_sep_str = sep_str;
      if (!search_trie(&p_sep_str, a, &result))
         build_trie(p_sep_str, result, &result);
   }
   else
      result = a;

   // Append b
   const char *bstr = istr(b);
   if (!search_trie(&bstr, result, &result))
      build_trie(bstr, result, &result);

   return result;
}

ident_t ident_strip(ident_t a, ident_t b)
{
   assert(a != NULL);
   assert(b != NULL);

   while (a->value == b->value && b->value != '\0') {
      a = a->up;
      b = b->up;
   }

   return (b->value == '\0' ? a : NULL);
}

bool ident_starts_with(ident_t a, ident_t b)
{
   while (a != b && a->value != '\0')
      a = a->up;

   return a == b;
}

char ident_char(ident_t i, unsigned n)
{
   if (i == NULL)
      return '\0';
   else if (n == 0)
      return i->value;
   else
      return ident_char(i->up, n - 1);
}

size_t ident_len(ident_t i)
{
   if (i == NULL || i->value == '\0')
      return 0;
   else
      return i->depth - 1;
}

static ident_t ident_suffix_until(ident_t i, char c, char escape1, char escape2)
{
   assert(i != NULL);

   bool escaping1 = false, escaping2 = false;
   ident_t r = i;
   while (i->value != '\0') {
      if (!escaping1 && !escaping2 && i->value == c)
         r = i->up;
      else if (i->value == escape1)
         escaping1 = !escaping1;
      else if (i->value == escape2)
         escaping2 = !escaping2;
      i = i->up;
   }

   return r;
}

ident_t ident_until(ident_t i, char c)
{
   return ident_suffix_until(i, c, '\0', '\0');
}

ident_t ident_runtil(ident_t i, char c)
{
   assert(i != NULL);

   for (ident_t r = i; r->value != '\0'; r = r->up) {
      if (r->value == c)
         return r->up;
   }

   return i;
}

ident_t ident_from(ident_t i, char c)
{
   assert(i != NULL);

   char buf[i->depth + 1];
   char *p = buf + i->depth;
   *p-- = '\0';

   char *from = NULL;
   while (i->value != '\0') {
      if (i->value == c)
         from = p + 1;
      *p-- = i->value;
      i = i->up;
   }

   return (from == NULL) ? NULL : ident_new(from);
}

ident_t ident_rfrom(ident_t i, char c)
{
   assert(i != NULL);

   char buf[i->depth + 1];
   char *p = buf + i->depth;
   *p-- = '\0';

   while (i->value != '\0') {
      if (i->value == c)
         return ident_new(p + 1);
      *p-- = i->value;
      i = i->up;
   }

   return NULL;
}

bool icmp(ident_t i, const char *s)
{
   if (i == NULL || s == NULL)
      return i == NULL && s == NULL;

   trie_t *result;
   if (!search_trie(&s, &(root.trie), &result))
      return false;
   else
      return result == i;
}

int ident_compare(ident_t a, ident_t b)
{
   if (a->up == b->up)
      return a->value - b->value;
   else if (a->depth > b->depth) {
      int cmp = ident_compare(a->up, b);
      return cmp == 0 ? a->value : cmp;
   }
   else if (b->depth > a->depth) {
      int cmp = ident_compare(a, b->up);
      return cmp == 0 ? 0 - b->value : cmp;
   }
   else
      return ident_compare(a->up, b->up);
}

static bool ident_glob_walk(const trie_t *i, const char *g,
                            const char *const end)
{
   if (i->value == '\0')
      return (g < end);
   else if (g < end)
      return false;
   else if (*g == '*')
      return ident_glob_walk(i->up, g, end)
         || ident_glob_walk(i->up, g - 1, end);
   else if (i->value == *g)
      return ident_glob_walk(i->up, g - 1, end);
   else
      return false;
}

bool ident_glob(ident_t i, const char *glob, int length)
{
   assert(i != NULL);

   if (length < 0)
      length = strlen(glob);

   return ident_glob_walk(i, glob + length - 1, glob);
}

bool ident_contains(ident_t i, const char *search)
{
   assert(i != NULL);

   for (ident_t r = i; r->value != '\0'; r = r->up) {
      for (const char *p = search; *p != '\0'; p++) {
         if (r->value == *p)
            return true;
      }
   }

   return false;
}

ident_t ident_downcase(ident_t i)
{
   // TODO: this could be implemented more efficiently

   if (i == NULL)
      return NULL;

   char *p = get_fmt_buf(i->depth) + i->depth - 1;
   *p = '\0';

   trie_t *it;
   for (it = i; it->value != '\0'; it = it->up)
      *(--p) = tolower((int)it->value);

   return ident_new(p);
}

ident_t ident_walk_selected(ident_t *i)
{
   if (*i == NULL)
      return NULL;

   ident_t result = ident_suffix_until(*i, '.', '\'', '\\');
   if (result == NULL || result == *i) {
      result = *i;
      *i = NULL;
   }
   else {
      char *LOCAL buf = xmalloc((*i)->depth + 1), *p = buf + (*i)->depth + 1;
      *--p = '\0';
      for (ident_t it = *i; it != result; it = it->up)
         *--p = it->value;
      *i = ident_new(p + 1);
   }

   return result;
}

int ident_distance(ident_t a, ident_t b)
{
   const int n = ident_len(b);
   const int m = ident_len(a);

   char s[m + 1], t[n + 1];
   istr_r(a, s, m + 1);
   istr_r(b, t, n + 1);

   int mem[2 * (n + 1)], *v0 = mem, *v1 = mem + n + 1;

   for (int i = 0; i <= n; i++)
      v0[i] = i;

   for (int i = 0; i < m; i++) {
      v1[0] = i + 1;

      for (int j = 0; j < n; j++) {
         const int dc = v0[j + 1] + 1;
         const int ic = v1[j] + 1;
         const int sc = (s[i] == t[j] ? v0[j] : v0[j] + 1);

         v1[j + 1] = MIN(dc, MIN(ic, sc));
      }

      int *tmp = v0;
      v0 = v1;
      v1 = tmp;
   }

   return v0[n];
}
