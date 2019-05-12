#include "util/printer.hpp"
#include "test_util.h"

#include <cstdio>

START_TEST(test_buffer_print)
{
   BufferPrinter p;

   p.print("Hello, World");
   ck_assert_str_eq("Hello, World", p.buffer());

   p.print("xxxx");
   ck_assert_str_eq("Hello, Worldxxxx", p.buffer());

   p.clear();
   ck_assert_str_eq("", p.buffer());

   p.color_print("$red$hello$$");
   ck_assert_str_eq("hello", p.buffer());

   p.clear();
   p.color_print("$red$");
   p.color_print("test");
   p.color_print("$$");
   ck_assert_str_eq("test", p.buffer());
}
END_TEST

START_TEST(test_buffer_overflow)
{
   BufferPrinter p(5);

   p.print("%d", 12345678);
   ck_assert_str_eq("12345678", p.buffer());
}
END_TEST

START_TEST(test_color_print)
{
   char *ptr = NULL;
   size_t size = 0;
   FILE *mem = open_memstream(&ptr, &size);
   ck_assert_ptr_nonnull(mem);

   TerminalPrinter p(mem, true);

   p.color_print("$red$hello$$");
   p.flush();
   ck_assert_str_eq("\033[31mhello\033[0m", ptr);

   rewind(mem);

   p.color_print("$$foo$red$bar$$baz");
   p.flush();
   ck_assert_str_eq("\033[0mfoo\033[31mbar\033[0mbaz", ptr);

   fclose(mem);
   free(ptr);
}
END_TEST

extern "C" Suite *get_printer_tests(void)
{
   Suite *s = suite_create("printer");

   TCase *tc = nvc_unit_test();
   tcase_add_test(tc, test_buffer_print);
   tcase_add_test(tc, test_buffer_overflow);
   tcase_add_test(tc, test_color_print);
   suite_add_tcase(s, tc);

   return s;
}
