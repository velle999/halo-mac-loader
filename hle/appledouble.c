// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

#define _GNU_SOURCE

#include "appledouble.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// All fields are big-endian.
#define APPLEDOUBLE_MAGIC 0x00051607
#define APPLEDOUBLE_HEADER_SIZE 26
#define APPLEDOUBLE_ENTRY_SIZE 12
#define ENTRY_RESOURCE_FORK 2
#define ENTRY_FINDER_INFO 9

static uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

int hle_appledouble_is_companion(const char* name) {
  return name[0] == '.' && name[1] == '_';
}

int hle_appledouble_read(const char* path, hle_appledouble* out) {
  memset(out, 0, sizeof(*out));
  const char* slash = strrchr(path, '/');
  int n;
  if (slash) {
    n = snprintf(out->path, sizeof(out->path), "%.*s/._%s",
                 (int)(slash - path), path, slash + 1);
  } else {
    n = snprintf(out->path, sizeof(out->path), "._%s", path);
  }
  if (n < 0 || (size_t)n >= sizeof(out->path)) {
    out->path[0] = '\0';
    return 0;
  }

  int fd = open(out->path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return 0;
  }
  uint8_t header[APPLEDOUBLE_HEADER_SIZE];
  int ok = pread(fd, header, sizeof(header), 0) == (ssize_t)sizeof(header) &&
           be32(header) == APPLEDOUBLE_MAGIC;
  unsigned entries = ok ? ((unsigned)header[24] << 8) | header[25] : 0;
  off_t file_size = lseek(fd, 0, SEEK_END);
  for (unsigned i = 0; ok && i < entries; i++) {
    uint8_t entry[APPLEDOUBLE_ENTRY_SIZE];
    if (pread(fd, entry, sizeof(entry),
              APPLEDOUBLE_HEADER_SIZE + i * APPLEDOUBLE_ENTRY_SIZE) !=
        (ssize_t)sizeof(entry)) {
      ok = 0;
      break;
    }
    uint32_t id = be32(entry);
    uint32_t offset = be32(entry + 4);
    uint32_t length = be32(entry + 8);
    if ((off_t)offset + (off_t)length > file_size) {
      continue;  // truncated companion: ignore the entry
    }
    if (id == ENTRY_RESOURCE_FORK) {
      out->has_resource_fork = 1;
      out->resource_offset = offset;
      out->resource_length = length;
    } else if (id == ENTRY_FINDER_INFO && length >= 32) {
      out->has_finder_info =
          pread(fd, out->finder_info, 32, offset) == 32;
    }
  }
  close(fd);
  return ok;
}
