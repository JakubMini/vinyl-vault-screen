#include <unity.h>

#include "../../include/tag_gate.h"

void setUp() {}
void tearDown() {}

static const char *A = "04C67BA5892681", *B = "04AABBCCDDEEFF";

static void a_new_tag_counts_once_while_it_stays() {
  TagGate g;
  TEST_ASSERT_TRUE(g.read(A, 1000));
  for (uint32_t t = 1300; t < 120000; t += 300) TEST_ASSERT_FALSE(g.read(A, t));  // two minutes on the stand
}

static void a_tag_counts_again_only_after_30_s_away() {
  TagGate g;
  TEST_ASSERT_TRUE(g.read(A, 1000));
  TEST_ASSERT_FALSE(g.read(A, 1000 + 29000));  // lifted for 29 s
  TEST_ASSERT_TRUE(g.read(A, 30000 + 31000));  // then for 31 s
}

static void another_tag_in_between_lets_it_count_again() {
  TagGate g;
  TEST_ASSERT_TRUE(g.read(A, 1000));
  TEST_ASSERT_TRUE(g.read(B, 3000));
  TEST_ASSERT_FALSE(g.read(B, 3300));
  TEST_ASSERT_TRUE(g.read(A, 5000));
}

static void the_clock_wrapping_does_not_fire_it() {
  TagGate g;
  TEST_ASSERT_TRUE(g.read(A, 0xFFFFFF00u));
  TEST_ASSERT_FALSE(g.read(A, 0x00000100u));  // millis() wrapped, 512 ms later
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(a_new_tag_counts_once_while_it_stays);
  RUN_TEST(a_tag_counts_again_only_after_30_s_away);
  RUN_TEST(another_tag_in_between_lets_it_count_again);
  RUN_TEST(the_clock_wrapping_does_not_fire_it);
  return UNITY_END();
}
