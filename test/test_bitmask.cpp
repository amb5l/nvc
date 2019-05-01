#include "util/bitmask.hpp"

#include <gtest/gtest.h>
#include <set>

TEST(Bitmask, basic) {
   Bitmask b(100);

   EXPECT_EQ(100, b.size());
   b.set(5);
   EXPECT_TRUE(b.is_set(5));
   b.set(80);
   EXPECT_TRUE(b.is_set(80));
}

TEST(Bitmask, first_clear) {
   Bitmask b(100);

   EXPECT_EQ(0, b.first_clear());
   b.set(0);
   EXPECT_EQ(1, b.first_clear());

   b.one();
   EXPECT_EQ(-1, b.first_clear());

   b.clear(78);
   EXPECT_EQ(78, b.first_clear());

   b.zero();
   for (size_t i = 0; i < b.size(); i++)
      b.set(i);
   EXPECT_EQ(-1, b.first_clear());
}

TEST(Bitmask, first_set) {
   Bitmask b(100);

   EXPECT_EQ(-1, b.first_set());
   b.set(0);
   EXPECT_EQ(0, b.first_set());

   b.zero();
   b.set(78);
   EXPECT_EQ(78, b.first_set());

   b.one();
   for (size_t i = 0; i < b.size(); i++)
      b.clear(i);
   EXPECT_EQ(-1, b.first_set());
}

TEST(Bitmask, all_set_clear) {
   Bitmask b(100);

   b.set(68);
   EXPECT_FALSE(b.all_clear());
   EXPECT_FALSE(b.all_set());

   b.one();
   EXPECT_TRUE(b.all_set());

   b.zero();
   EXPECT_TRUE(b.all_clear());
}

TEST(Bitmask, random) {
   Bitmask b(512);

   EXPECT_EQ(512, b.size());

   for (size_t i = 0; i < b.size(); i++)
      EXPECT_FALSE(b.is_set(i));

   std::set<int> bits;
   for (int i = 0; i < 200; i++) {
      const int n = random() % b.size();
      bits.insert(n);
      b.set(n);
   }

   for (size_t i = 0; i < b.size(); i++) {
      if (bits.find(i) != bits.end())
         EXPECT_TRUE(b.is_set(i)) << "expected bit " << i << " set";
      else
         EXPECT_FALSE(b.is_set(i)) << "expected bit " << i << " clear";
   }
}
