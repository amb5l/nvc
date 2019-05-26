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

extern "C" Suite *get_util_tests(void)
{
   Suite *s = suite_create("util");

   TCase *tc = nvc_unit_test("array");
   tcase_add_test(tc, test_array1);
   suite_add_tcase(s, tc);

   return s;
}
