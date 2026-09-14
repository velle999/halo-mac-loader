// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Ranges in an array sorted by base: found by a binary search, added and
// removed by moving the entries after them. The game has a few thousand
// vertex buffers at most, and adds and removes them as levels load.

#include "var_ranges.h"

#include <stdlib.h>
#include <string.h>

// The index of the first range whose base is above |address|.
static size_t upper_bound(const hle_var_ranges* map, uintptr_t address) {
  size_t lo = 0;
  size_t hi = map->count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (map->ranges[mid].base <= address) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

static uintptr_t end_of(const hle_var_range* r) {
  return r->base + r->length;
}

int hle_var_ranges_add(hle_var_ranges* map, uintptr_t base, size_t length,
                       uint32_t value) {
  if (!length || base + length < base) {
    return 0;
  }
  size_t i = upper_bound(map, base);
  if ((i > 0 && end_of(&map->ranges[i - 1]) > base) ||
      (i < map->count && map->ranges[i].base < base + length)) {
    return 0;
  }
  if (map->count == map->capacity) {
    size_t capacity = map->capacity ? map->capacity * 2 : 64;
    hle_var_range* grown = realloc(map->ranges, capacity * sizeof(*grown));
    if (!grown) {
      return 0;
    }
    map->ranges = grown;
    map->capacity = capacity;
  }
  memmove(&map->ranges[i + 1], &map->ranges[i],
          (map->count - i) * sizeof(*map->ranges));
  map->ranges[i] = (hle_var_range){ base, length, value };
  map->count++;
  return 1;
}

int hle_var_ranges_remove(hle_var_ranges* map, uintptr_t base,
                          uint32_t* value) {
  size_t i = upper_bound(map, base);
  if (i == 0 || map->ranges[i - 1].base != base) {
    return 0;
  }
  if (value) {
    *value = map->ranges[i - 1].value;
  }
  memmove(&map->ranges[i - 1], &map->ranges[i],
          (map->count - i) * sizeof(*map->ranges));
  map->count--;
  return 1;
}

size_t hle_var_ranges_take_overlapping(hle_var_ranges* map, uintptr_t base,
                                       size_t length, uint32_t* values,
                                       size_t max) {
  if (!length) {
    return 0;
  }
  uintptr_t end = base + length < base ? UINTPTR_MAX : base + length;
  size_t first = upper_bound(map, base);
  if (first > 0 && end_of(&map->ranges[first - 1]) > base) {
    first--;
  }
  size_t last = first;
  while (last < map->count && map->ranges[last].base < end) {
    last++;
  }
  size_t taken = last - first;
  for (size_t i = 0; i < taken && i < max; i++) {
    values[i] = map->ranges[first + i].value;
  }
  memmove(&map->ranges[first], &map->ranges[last],
          (map->count - last) * sizeof(*map->ranges));
  map->count -= taken;
  return taken;
}

const hle_var_range* hle_var_ranges_find(const hle_var_ranges* map,
                                         uintptr_t address) {
  size_t i = upper_bound(map, address);
  if (i == 0) {
    return NULL;
  }
  const hle_var_range* r = &map->ranges[i - 1];
  return address - r->base < r->length ? r : NULL;
}

void hle_var_ranges_free(hle_var_ranges* map) {
  free(map->ranges);
  memset(map, 0, sizeof(*map));
}
