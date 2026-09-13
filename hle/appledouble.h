// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// AppleDouble companion files. A Mac file copied to a filesystem without
// forks keeps its resource fork and Finder info in "._name" beside it; the
// game's EULA.rsrc is an empty data fork with its resources in ._EULA.rsrc.

#ifndef HLE_APPLEDOUBLE_H_
#define HLE_APPLEDOUBLE_H_

#include <limits.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct {
  // The "._name" path, whether or not it exists.
  char path[PATH_MAX];
  int has_finder_info;
  // As stored: big-endian FInfo then FXInfo.
  uint8_t finder_info[32];
  int has_resource_fork;
  off_t resource_offset;
  uint32_t resource_length;
} hle_appledouble;

// Fills |out| from the companion of |path|. Returns 1 when it exists and
// parses; 0 otherwise, with |out->path| still set.
int hle_appledouble_read(const char* path, hle_appledouble* out);

// True for names that are AppleDouble companions, which a Mac never lists.
int hle_appledouble_is_companion(const char* name);

#endif  // HLE_APPLEDOUBLE_H_
