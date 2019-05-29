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

#include "util/array.hpp"
#include "util/stacktrace.hpp"
#include "util/hash.hpp"
#include "test_util.h"

START_TEST(test_array1)
{
   ArrayList<int> array;
   ck_assert_int_eq(0, array.size());

   array.add(1).add(2).add(3);
   ck_assert_int_eq(3, array.size());

   ck_assert_int_eq(1, array.get(0));
   ck_assert_int_eq(2, array.get(1));
   ck_assert_int_eq(3, array.get(2));

   ck_assert_int_eq(1, array[0]);
   ck_assert_int_eq(2, array[1]);
   ck_assert_int_eq(3, array[2]);

   int check = 1;
   for (int value : array) {
      ck_assert_int_eq(check, value);
      check++;
   }
}
END_TEST

START_TEST(test_array2)
{
   static int copies = 0, moves = 0, assigns = 0;
   static int constructs = 0, destructs = 0;

   struct Counter {
      Counter() { constructs++; }
      Counter(const Counter&) { copies++; }
      Counter(Counter&&) { moves++; }
      ~Counter() { destructs++; }

      Counter& operator=(const Counter&) { assigns++; return *this; }
   };

   {
      ArrayList<Counter> list;

      list.add(Counter()).add(Counter());

      ck_assert_int_eq(2, constructs);
      ck_assert_int_eq(2, destructs);
      ck_assert_int_eq(0, copies);
      ck_assert_int_eq(2, moves);
      ck_assert_int_eq(0, assigns);

      for (const Counter& c: list)
         (void)c;

      ck_assert_int_eq(2, constructs);
      ck_assert_int_eq(2, destructs);
      ck_assert_int_eq(0, copies);
      ck_assert_int_eq(2, moves);
      ck_assert_int_eq(0, assigns);

      for (int i = 0; i < 20; i++)
         list.add(Counter());

      ck_assert_int_eq(22, constructs);
      ck_assert_int_eq(38, destructs);
      ck_assert_int_eq(0, copies);
      ck_assert_int_eq(38, moves);
      ck_assert_int_eq(0, assigns);
   }

   ck_assert_int_eq(60, destructs);
}
END_TEST

static int test_two_line, test_one_line;

__attribute__((noinline))
FrameList trace_test_two()
{
   test_two_line = __LINE__; return stack_trace();
}

__attribute__((noinline))
FrameList trace_test_one()
{
   test_one_line = __LINE__; return trace_test_two();
}

START_TEST(test_trace1)
{
   FrameList result = trace_test_one();

   ck_assert_int_ge(result.size(), 3);
   ck_assert_str_eq("trace_test_two()", result[0].symbol());
   ck_assert_str_eq("trace_test_one()", result[1].symbol());
   ck_assert_str_eq("main", result[result.size() - 1].symbol());

   ck_assert_int_eq(test_two_line, result[0].line());
   ck_assert_int_eq(test_one_line, result[1].line());
}
END_TEST

START_TEST(test_hash_basic)
{
   HashMap<int, int> h(8);

   h.put(1516, 6);
   h.put(151670, 4);
   h.put(61, 1);

   fail_unless(h.get(1516) == 6);
   fail_unless(h.get(151670) == 4);
   fail_unless(h.get(61) == 1);

   ck_assert_int_eq(0, h.get(55));
}
END_TEST;

START_TEST(test_hash_rand)
{
   static const int N = 1024;

   HashMap<int, int> h;
   int keys[N];
   int values[N];

   for (int i = 0; i < N; i++) {
      do {
         keys[i] = ((i << 16) | (rand() & 0xffff));
      } while (keys[i] == 0);
      values[i] = rand();
   }

   for (int i = 0; i < N; i++)
      h.put(keys[i], values[i]);

   for (int i = 0; i < N; i++)
      fail_unless(h.get(keys[i]) == values[i]);
}
END_TEST;

extern "C" Suite *get_util_tests(void)
{
   Suite *s = suite_create("util");

   TCase *tc_array = nvc_unit_test("array");
   tcase_add_test(tc_array, test_array1);
   tcase_add_test(tc_array, test_array2);
   suite_add_tcase(s, tc_array);

   TCase *tc_trace = nvc_unit_test("stacktrace");
   tcase_add_test(tc_trace, test_trace1);
   suite_add_tcase(s, tc_trace);

   TCase *tc_hash = nvc_unit_test("hash");
   tcase_add_test(tc_hash, test_hash_basic);
   tcase_add_test(tc_hash, test_hash_rand);
   suite_add_tcase(s, tc_trace);

   return s;
}
