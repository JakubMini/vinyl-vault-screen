// The letters on the crate's bar, kept free of Arduino so it can be tested on the laptop.
// One per first character of the artists, in crate order, each with the first record it starts.
// Worked out from the records each time, so a new artist's letter turns up after the next sync.
// Crate order, not A to Z: the vault sorts "Ólafur" after "W", and that is where the bar takes you.
#pragma once
#include <string.h>

#include <vector>

struct CrateLetter {
  char text[5];  // the first character in UTF-8, a-z upper-cased
  int first;     // the first record whose artist starts with it
};

// The first character of s, whole even when it takes several UTF-8 bytes.
inline void first_letter(const char *s, char out[5]) {
  const unsigned char c = s[0];
  const int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
  int i = 0;
  for (; i < n && s[i]; i++) out[i] = s[i];
  out[i] = 0;
  if (out[0] >= 'a' && out[0] <= 'z') out[0] -= 'a' - 'A';
}

// artist(i) gives the i-th record's artist as a C string.
template <class Artist>
std::vector<CrateLetter> crate_letters(int count, Artist artist) {
  std::vector<CrateLetter> out;
  for (int i = 0; i < count; i++) {
    CrateLetter l;
    first_letter(artist(i), l.text);
    if (!l.text[0]) continue;  // no artist
    bool seen = false;
    for (const CrateLetter &o : out) seen |= strcmp(o.text, l.text) == 0;
    if (!seen) l.first = i, out.push_back(l);
  }
  return out;
}

// The letter under a finger at x on a bar from left, width wide, its letters spaced evenly.
inline int letter_at(int x, int left, int width, int letters) {
  const int k = (x - left) * letters / width;
  return k < 0 ? 0 : k >= letters ? letters - 1 : k;
}
