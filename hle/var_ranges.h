// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Address ranges that do not overlap, each with a value, found by an address
// inside them: the vertex array ranges of gl_var.c.

#ifndef HLE_VAR_RANGES_H_
#define HLE_VAR_RANGES_H_

#include <stddef.h>
#include <stdint.h>

typedef struct {
  uintptr_t base;
  size_t length;
  uint32_t value;
} hle_var_range;

// Sorted by base. Starts zeroed.
typedef struct {
  hle_var_range* ranges;
  size_t count;
  size_t capacity;
} hle_var_ranges;

// Adds [base, base + length) with |value|. Returns 0, adding nothing, when
// the range is empty, wraps, overlaps one already there, or memory runs out.
int hle_var_ranges_add(hle_var_ranges* map, uintptr_t base, size_t length,
                       uint32_t value);

// Removes the range starting at |base| and sets |*value| to its value.
// Returns 0 when no range starts there.
int hle_var_ranges_remove(hle_var_ranges* map, uintptr_t base,
                          uint32_t* value);

// Removes every range overlapping [base, base + length), writing the values
// of the first |max| to |values| in address order. Returns how many were
// removed.
size_t hle_var_ranges_take_overlapping(hle_var_ranges* map, uintptr_t base,
                                       size_t length, uint32_t* values,
                                       size_t max);

// The range holding |address|, or NULL. Valid until the map changes.
const hle_var_range* hle_var_ranges_find(const hle_var_ranges* map,
                                         uintptr_t address);

void hle_var_ranges_free(hle_var_ranges* map);

#endif  // HLE_VAR_RANGES_H_
