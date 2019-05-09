#include "util/bitmask.hpp"
#include "test_util.h"

#include <set>

START_TEST(test_basic) {
   Bitmask b(100);

   fail_unless(100 == b.size());
   b.set(5);
   fail_unless(b.is_set(5));
   b.set(80);
   fail_unless(b.is_set(80));
}
END_TEST

START_TEST(test_first_clear) {
   Bitmask b(100);

   fail_unless(0 == b.first_clear());
   b.set(0);
   fail_unless(1 == b.first_clear());

   b.one();
   fail_unless(-1 == b.first_clear());

   b.clear(78);
   fail_unless(78 == b.first_clear());

   b.zero();
   for (size_t i = 0; i < b.size(); i++)
      b.set(i);
   fail_unless(-1 == b.first_clear());
}
END_TEST

START_TEST(test_first_set) {
   Bitmask b(100);

   fail_unless(-1 == b.first_set());
   b.set(0);
   fail_unless(0 == b.first_set());

   b.zero();
   b.set(78);
   fail_unless(78 == b.first_set());

   b.one();
   for (size_t i = 0; i < b.size(); i++)
      b.clear(i);
   fail_unless(-1 == b.first_set());
}
END_TEST

START_TEST(test_all_set_clear) {
   Bitmask b(100);

   b.set(68);
   fail_if(b.all_clear());
   fail_if(b.all_set());

   b.one();
   fail_unless(b.all_set());

   b.zero();
   fail_unless(b.all_clear());
}
END_TEST

START_TEST(test_random) {
   Bitmask b(512);

   fail_unless(512 == b.size());

   for (size_t i = 0; i < b.size(); i++)
      fail_if(b.is_set(i));

   std::set<int> bits;
   for (int i = 0; i < 200; i++) {
      const int n = random() % b.size();
      bits.insert(n);
      b.set(n);
   }

   for (size_t i = 0; i < b.size(); i++) {
      if (bits.find(i) != bits.end())
         fail_unless(b.is_set(i), "expected bit %d set", i);
      else
         fail_if(b.is_set(i), "expected bit %d clear", i);
   }
}
END_TEST

extern "C" Suite *get_bitmask_tests(void)
{
   Suite *s = suite_create("bitmask");

   TCase *tc = nvc_unit_test();
   tcase_add_test(tc, test_basic);
   tcase_add_test(tc, test_random);
   tcase_add_test(tc, test_all_set_clear);
   tcase_add_test(tc, test_first_set);
   tcase_add_test(tc, test_first_clear);
   suite_add_tcase(s, tc);

   return s;
}
