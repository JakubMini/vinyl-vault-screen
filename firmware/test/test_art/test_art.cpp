#include <unity.h>

#include "../../include/record_art.h"

void setUp() {}
void tearDown() {}

static void the_parts_sit_where_the_site_draws_them() {
  const int n = 240;
  TEST_ASSERT_EQUAL_HEX16(ART_BG, record_px(n / 2, n / 2, n, 0xFFFF));       // spindle hole
  TEST_ASSERT_EQUAL_HEX16(ART_LABEL, record_px(n / 2 + 20, n / 2, n, 0xFFFF));  // label
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, record_px(0, 0, n, 0xFFFF));                // corner, off the disc
  TEST_ASSERT_EQUAL_HEX16(0xFFFF, record_px(-5, n / 2, n, 0xFFFF));           // beyond the square
}

static void the_vinyl_stays_dark_and_the_sheen_is_on_the_diagonal() {
  const int n = 240;
  const uint16_t side = record_px(n / 2 + 85, n / 2 + 1, n, 0);  // right of centre, off the diagonal
  const uint16_t sheen = record_px(n / 2 - 60, n / 2 - 60, n, 0);  // upper-left, on it
  TEST_ASSERT_TRUE((side >> 11) < 3);
  TEST_ASSERT_TRUE((sheen >> 11) > (side >> 11));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(the_parts_sit_where_the_site_draws_them);
  RUN_TEST(the_vinyl_stays_dark_and_the_sheen_is_on_the_diagonal);
  return UNITY_END();
}
