// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Checks hle/var_ranges.c: ranges are found by the addresses inside them and
// no others, overlaps are refused or taken whole, and a long run of random
// changes agrees with a map of every address.
//
//   make tests/var_ranges_test && tests/var_ranges_test

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../hle/var_ranges.h"

static int failures;

static void check(int ok, const char* what) {
  if (!ok) {
    printf("FAIL: %s\n", what);
    failures++;
  }
}

static int found(const hle_var_ranges* map, uintptr_t address,
                 uint32_t value) {
  const hle_var_range* r = hle_var_ranges_find(map, address);
  return r && r->value == value;
}

static void test_examples(void) {
  hle_var_ranges map = { 0 };
  check(hle_var_ranges_add(&map, 0x1000, 0x100, 1), "a range is added");
  check(hle_var_ranges_add(&map, 0x2000, 0x1000, 2), "a second is added");
  check(hle_var_ranges_add(&map, 0x1100, 0x100, 3),
        "a range just after another is added");
  check(found(&map, 0x1000, 1) && found(&map, 0x10ff, 1),
        "a range holds its first and last address");
  check(found(&map, 0x1100, 3) && found(&map, 0x11ff, 3),
        "the next range starts where the last ends");
  check(!hle_var_ranges_find(&map, 0xfff) &&
            !hle_var_ranges_find(&map, 0x1200) &&
            !hle_var_ranges_find(&map, 0x3000),
        "addresses around the ranges are in none");
  check(found(&map, 0x2fff, 2), "the last address of the second range");

  check(!hle_var_ranges_add(&map, 0x10f0, 0x20, 4),
        "a range overlapping two is refused");
  check(!hle_var_ranges_add(&map, 0x1000, 0x10, 4),
        "a range at another's base is refused");
  check(!hle_var_ranges_add(&map, 0x1fff, 2, 4),
        "a range reaching into another is refused");
  check(!hle_var_ranges_add(&map, 0x5000, 0, 4), "an empty range is refused");
  check(!hle_var_ranges_add(&map, UINTPTR_MAX - 4, 16, 4),
        "a range that wraps is refused");

  uint32_t values[4] = { 0 };
  size_t taken = hle_var_ranges_take_overlapping(&map, 0x10f0, 0x20, values, 4);
  check(taken == 2 && values[0] == 1 && values[1] == 3,
        "the ranges an overlap touches are taken, in order");
  check(!hle_var_ranges_find(&map, 0x1000) && found(&map, 0x2000, 2),
        "taken ranges are gone and the rest stay");

  uint32_t value = 0;
  check(hle_var_ranges_remove(&map, 0x2000, &value) && value == 2,
        "a range is removed by its base");
  check(!hle_var_ranges_remove(&map, 0x2000, &value),
        "a range is removed once");
  check(map.count == 0, "the map is empty");
  hle_var_ranges_free(&map);
}

// Addresses 0 to kSpace - 1, each noting the value of the range holding it
// plus one, or 0.
enum { kSpace = 1 << 16 };
static uint32_t model[kSpace];

static uint32_t next_random(uint32_t* state) {
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return *state = x;
}

static void test_random(void) {
  hle_var_ranges map = { 0 };
  uint32_t state = 0x9e3779b9;
  uint32_t bases[1024];
  size_t lengths[1024];
  int live[1024] = { 0 };
  int agreed = 1;
  for (int step = 0; step < 20000 && agreed; step++) {
    uint32_t slot = next_random(&state) % 1024;
    if (!live[slot]) {
      uintptr_t base = next_random(&state) % (kSpace - 600);
      size_t length = 1 + next_random(&state) % 512;
      int free_space = 1;
      for (size_t a = base; a < base + length; a++) {
        free_space &= model[a] == 0;
      }
      int added = hle_var_ranges_add(&map, base, length, slot);
      if (added != free_space) {
        agreed = 0;
        break;
      }
      if (added) {
        for (size_t a = base; a < base + length; a++) {
          model[a] = slot + 1;
        }
        bases[slot] = base;
        lengths[slot] = length;
        live[slot] = 1;
      }
    } else if (next_random(&state) % 4) {
      uint32_t value = 0;
      if (!hle_var_ranges_remove(&map, bases[slot], &value) || value != slot) {
        agreed = 0;
        break;
      }
      for (size_t a = bases[slot]; a < bases[slot] + lengths[slot]; a++) {
        model[a] = 0;
      }
      live[slot] = 0;
    } else {
      uintptr_t base = next_random(&state) % (kSpace - 600);
      size_t length = 1 + next_random(&state) % 600;
      uint32_t values[1024];
      size_t taken =
          hle_var_ranges_take_overlapping(&map, base, length, values, 1024);
      size_t expected = 0;
      for (uint32_t s = 0; s < 1024; s++) {
        if (live[s] && bases[s] < base + length &&
            bases[s] + lengths[s] > base) {
          expected++;
          live[s] = 0;
          for (size_t a = bases[s]; a < bases[s] + lengths[s]; a++) {
            model[a] = 0;
          }
        }
      }
      if (taken != expected) {
        agreed = 0;
        break;
      }
    }
    for (int probe = 0; probe < 8; probe++) {
      uintptr_t address = next_random(&state) % kSpace;
      const hle_var_range* r = hle_var_ranges_find(&map, address);
      if ((r ? r->value + 1 : 0) != model[address]) {
        agreed = 0;
        break;
      }
    }
  }
  check(agreed, "20000 random changes agree with a map of every address");
  hle_var_ranges_free(&map);
}

int main(void) {
  test_examples();
  test_random();
  if (failures) {
    printf("%d failed\n", failures);
    return 1;
  }
  printf("all passed\n");
  return 0;
}
