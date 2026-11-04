#include <unity.h>

#include "../../include/crate_letters.h"

void setUp() {}
void tearDown() {}

static const char *CRATE[] = {"ABBA", "Alt-J", "", "bob marley", "Bruce Springsteen", "Doors", "Wojciech Młynarski",
                              "Ólafur Arnalds", "Ólafur Arnalds & Alice Sara Ott", "Brenda Lee"};
static const int N = sizeof(CRATE) / sizeof(*CRATE);

static std::vector<CrateLetter> letters() {
  return crate_letters(N, [](int i) { return CRATE[i]; });
}

static void one_letter_each_in_crate_order_at_its_first_record() {
  const auto l = letters();
  TEST_ASSERT_EQUAL(5, l.size());
  const char *text[] = {"A", "B", "D", "W", "Ó"};
  const int first[] = {0, 3, 5, 6, 7};
  for (int i = 0; i < 5; i++) {
    TEST_ASSERT_EQUAL_STRING(text[i], l[i].text);
    TEST_ASSERT_EQUAL(first[i], l[i].first);
  }
}

static void lower_case_joins_its_capital_and_empty_artists_are_skipped() {
  const auto l = letters();
  TEST_ASSERT_EQUAL_STRING("B", l[1].text);  // "bob marley"
  for (const auto &x : l) TEST_ASSERT_NOT_EQUAL(0, x.text[0]);
}

static void a_multibyte_first_letter_stays_whole() {
  char out[5];
  first_letter("Łzy", out);
  TEST_ASSERT_EQUAL_STRING("Ł", out);
  first_letter("Ó", out);
  TEST_ASSERT_EQUAL_STRING("Ó", out);
  first_letter("\xC3", out);  // cut short: no reading past the end
  TEST_ASSERT_EQUAL_STRING("\xC3", out);
}

static void a_finger_picks_evenly_spaced_letters_clamped_at_the_ends() {
  TEST_ASSERT_EQUAL(0, letter_at(0, 16, 448, 21));
  TEST_ASSERT_EQUAL(0, letter_at(16, 16, 448, 21));
  TEST_ASSERT_EQUAL(10, letter_at(16 + 224, 16, 448, 21));
  TEST_ASSERT_EQUAL(20, letter_at(16 + 447, 16, 448, 21));
  TEST_ASSERT_EQUAL(20, letter_at(479, 16, 448, 21));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(one_letter_each_in_crate_order_at_its_first_record);
  RUN_TEST(lower_case_joins_its_capital_and_empty_artists_are_skipped);
  RUN_TEST(a_multibyte_first_letter_stays_whole);
  RUN_TEST(a_finger_picks_evenly_spaced_letters_clamped_at_the_ends);
  return UNITY_END();
}
