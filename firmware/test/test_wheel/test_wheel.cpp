#include <unity.h>

#include "../../include/wheel.h"

void setUp() {}
void tearDown() {}

static void front_cover_is_centred_and_full_size() {
  WheelSlot s = wheel_slot(0);
  TEST_ASSERT_EQUAL_FLOAT(WHEEL_CX, s.x);
  TEST_ASSERT_EQUAL_FLOAT(WHEEL_CY, s.y);
  TEST_ASSERT_EQUAL_FLOAT(1.0f, s.scale);
  TEST_ASSERT_EQUAL_INT(255, s.opa);
}

static void neighbours_mirror_each_other() {
  WheelSlot before = wheel_slot(-1), after = wheel_slot(1);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 2 * WHEEL_CX, before.x + after.x);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, before.y, after.y);
  TEST_ASSERT_EQUAL_FLOAT(before.scale, after.scale);
}

static void covers_shrink_fade_and_drop_round_the_rim() {
  WheelSlot front = wheel_slot(0), one = wheel_slot(1), two = wheel_slot(2);
  TEST_ASSERT_TRUE(one.scale < front.scale && two.scale < one.scale);
  TEST_ASSERT_TRUE(one.opa < front.opa && two.opa < one.opa);
  TEST_ASSERT_TRUE(one.y > front.y && two.y > one.y);
  TEST_ASSERT_TRUE(wheel_slot(10).scale > 0 && wheel_slot(10).opa == 0);
}

static void the_covers_before_and_after_are_on_screen() {
  TEST_ASSERT_TRUE(wheel_slot(1).x < 480 && wheel_slot(-1).x > 0);
}

static void index_wraps_both_ways() {
  TEST_ASSERT_EQUAL_INT(9, wheel_index(-1, 10));
  TEST_ASSERT_EQUAL_INT(0, wheel_index(10, 10));
  TEST_ASSERT_EQUAL_INT(3, wheel_index(-17, 10));
}

static void spans_stay_sorted_and_merged() {
  Span s[8];
  int n = 0;
  n = spans_add(s, n, 100, 200);
  n = spans_add(s, n, 300, 400);
  n = spans_add(s, n, 0, 50);
  TEST_ASSERT_EQUAL_INT(3, n);
  TEST_ASSERT_EQUAL_INT(0, s[0].a);
  TEST_ASSERT_EQUAL_INT(300, s[2].a);
  n = spans_add(s, n, 150, 350);  // bridges the last two
  TEST_ASSERT_EQUAL_INT(2, n);
  TEST_ASSERT_EQUAL_INT(100, s[1].a);
  TEST_ASSERT_EQUAL_INT(400, s[1].b);
  n = spans_add(s, n, 50, 100);  // touching spans join
  TEST_ASSERT_EQUAL_INT(1, n);
  TEST_ASSERT_EQUAL_INT(0, s[0].a);
  TEST_ASSERT_EQUAL_INT(400, s[0].b);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(front_cover_is_centred_and_full_size);
  RUN_TEST(neighbours_mirror_each_other);
  RUN_TEST(covers_shrink_fade_and_drop_round_the_rim);
  RUN_TEST(the_covers_before_and_after_are_on_screen);
  RUN_TEST(index_wraps_both_ways);
  RUN_TEST(spans_stay_sorted_and_merged);
  return UNITY_END();
}
