//
//  Copyright (C) 2011-2022  Nick Gasson
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

#ifndef _IDENT_H
#define _IDENT_H

#include "prim.h"

// Intern a string as an identifier.
ident_t ident_new(const char *str);

// True if the given string was already interned.
bool ident_interned(const char *str);

// Generate a unique identifier with the given prefix.
ident_t ident_uniq(const char *prefix);

// Create a new identifier which is a prepended to b separated
// by a dot.
ident_t ident_prefix(ident_t a, ident_t b, char sep);

// Strips a suffix from an identifier or returns NULL if this
// is not possible.
ident_t ident_strip(ident_t a, ident_t b);

// True if identifier a starts with b
bool ident_starts_with(ident_t a, ident_t b);

// Return the Nth character of an identifier counting from the end.
char ident_char(ident_t i, unsigned n);

// Number of characters in the identifier
size_t ident_len(ident_t i);

// Return the prefix of i that does not include c
ident_t ident_until(ident_t i, char c);

// Return the prefix of i up to the final c
ident_t ident_runtil(ident_t i, char c);

// Return the suffix of i from the final c
ident_t ident_rfrom(ident_t i, char c);

// Return the suffix of i from the first c
ident_t ident_from(ident_t i, char c);

// Compare identifier against a NULL-terminated string
bool icmp(ident_t i, const char *s);

// Compare two identifiers for lexical ordering
int ident_compare(ident_t a, ident_t b);

// Compare identifier against a pattern containing wildcards
// Length is optional and may be set to -1 but improves performance
// if set to the length of glob
bool ident_glob(ident_t i, const char *glob, int length);

// True if identifier contains any characters from search
bool ident_contains(ident_t i, const char *search);

// Convert an indentifier to lower case
ident_t ident_downcase(ident_t i);

// Iterate through dot-separated name components
ident_t ident_walk_selected(ident_t *i);

// Convert an identifier reference to a NULL-terminated string.
// This function is quite slow so its use should be avoided except
// for printing.
const char *istr(ident_t ident);

// As above but write into a user supplied buffer.
void istr_r(ident_t ident, char *buf, size_t sz);

// Compute Levenshtein distance between two identifiers
int ident_distance(ident_t a, ident_t b);

ident_wr_ctx_t ident_write_begin(fbuf_t *f);
void ident_write(ident_t ident, ident_wr_ctx_t ctx);
void ident_write_end(ident_wr_ctx_t ctx);

ident_rd_ctx_t ident_read_begin(fbuf_t *f);
ident_t ident_read(ident_rd_ctx_t ctx);
void ident_read_end(ident_rd_ctx_t ctx);

#endif // _IDENT_H
