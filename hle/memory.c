// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// The Memory Manager's pointers and handles, and the Pascal string helpers.
// Blocks are 16-byte aligned, as Mac OS X's malloc returns them, so SSE code
// in the game can use aligned moves on them.

#define _GNU_SOURCE

#include <ctype.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "carbon.h"

static __thread OSErr mem_error;

void hle_set_mem_error(OSErr err) {
  mem_error = err;
}

int MemError(void) {
  return mem_error;
}

// ---------------------------------------------------------------------------
// Pointers: the size sits in a 16-byte header in front of the block.

#define PTR_MAGIC 0x50747221u

typedef struct {
  Size size;
  uint32_t magic;
  uint32_t pad[2];
} ptr_header;

static ptr_header* header_of(Ptr p) {
  ptr_header* h = (ptr_header*)p - 1;
  if (h->magic != PTR_MAGIC) {
    fprintf(stderr, "hle: %p did not come from NewPtr\n", (void*)p);
    abort();
  }
  return h;
}

Ptr NewPtr(Size size) {
  ptr_header* h;
  if (size < 0 ||
      posix_memalign((void**)&h, 16, sizeof(*h) + (size ? size : 1))) {
    mem_error = memFullErr;
    return NULL;
  }
  h->size = size;
  h->magic = PTR_MAGIC;
  mem_error = noErr;
  return (Ptr)(h + 1);
}

Ptr NewPtrClear(Size size) {
  Ptr p = NewPtr(size);
  if (p) {
    memset(p, 0, size);
  }
  return p;
}

void DisposePtr(Ptr p) {
  if (!p) {
    return;
  }
  ptr_header* h = header_of(p);
  h->magic = 0;
  free(h);
  mem_error = noErr;
}

Size GetPtrSize(Ptr p) {
  mem_error = noErr;
  return header_of(p)->size;
}

// Grows only within the block already allocated: a pointer cannot move.
void SetPtrSize(Ptr p, Size size) {
  ptr_header* h = header_of(p);
  size_t usable = malloc_usable_size(h) - sizeof(*h);
  if (size >= 0 && (size_t)size <= usable) {
    h->size = size;
    mem_error = noErr;
  } else {
    mem_error = memFullErr;
  }
}

// ---------------------------------------------------------------------------
// Handles: a Handle points at the master pointer, the first field here.

#define HANDLE_MAGIC 0x48646c21u

typedef struct {
  Ptr master;
  Size size;
  uint32_t magic;
  int is_resource;
} handle_block;

static handle_block* block_of(Handle h) {
  handle_block* b = (handle_block*)h;
  if (b->magic != HANDLE_MAGIC) {
    fprintf(stderr, "hle: %p is not a handle\n", (void*)h);
    abort();
  }
  return b;
}

Handle hle_handle_new(const void* data, Size size, int is_resource) {
  handle_block* b = calloc(1, sizeof(*b));
  if (!b || posix_memalign((void**)&b->master, 16, size > 0 ? size : 1)) {
    free(b);
    mem_error = memFullErr;
    return NULL;
  }
  if (data) {
    memcpy(b->master, data, size);
  } else {
    memset(b->master, 0, size > 0 ? size : 1);
  }
  b->size = size;
  b->magic = HANDLE_MAGIC;
  b->is_resource = is_resource;
  mem_error = noErr;
  return &b->master;
}

int hle_handle_is_resource(Handle h) {
  return block_of(h)->is_resource;
}

Handle NewHandle(Size size) {
  return size < 0 ? NULL : hle_handle_new(NULL, size, 0);
}

Handle NewHandleClear(Size size) {
  return NewHandle(size);
}

void DisposeHandle(Handle h) {
  if (!h) {
    mem_error = nilHandleErr;
    return;
  }
  handle_block* b = block_of(h);
  if (b->is_resource) {
    // Disposing of a resource directly: the Resource Manager must not hand
    // this handle out again.
    hle_resource_forget(h);
  }
  free(b->master);
  b->magic = 0;
  free(b);
  mem_error = noErr;
}

Size GetHandleSize(Handle h) {
  if (!h) {
    mem_error = nilHandleErr;
    return 0;
  }
  mem_error = noErr;
  return block_of(h)->size;
}

// A handle's block may move, so this one can grow.
void SetHandleSize(Handle h, Size size) {
  handle_block* b = block_of(h);
  Ptr grown;
  if (size < 0 || posix_memalign((void**)&grown, 16, size > 0 ? size : 1)) {
    mem_error = memFullErr;
    return;
  }
  memcpy(grown, b->master, b->size < size ? b->size : size);
  if (size > b->size) {
    memset(grown + b->size, 0, size - b->size);
  }
  free(b->master);
  b->master = grown;
  b->size = size;
  mem_error = noErr;
}

// Blocks never move, so locking has nothing to do.
void HLock(Handle h) {
  mem_error = h ? noErr : nilHandleErr;
}

void HUnlock(Handle h) {
  mem_error = h ? noErr : nilHandleErr;
}

// ---------------------------------------------------------------------------
// Pascal strings

void c2pstrcpy(unsigned char* dst, const char* src) {
  size_t n = strlen(src);
  if (n > 255) {
    n = 255;
  }
  memmove(dst + 1, src, n);
  dst[0] = n;
}

void p2cstrcpy(char* dst, const unsigned char* src) {
  size_t n = src[0];
  memmove(dst, src + 1, n);
  dst[n] = '\0';
}

// 0 when the strings match ignoring case, 1 otherwise. Only ASCII case is
// folded; the game compares file and volume names with it.
int IdenticalString(const unsigned char* a, const unsigned char* b,
                    Handle itl2) {
  if (a[0] != b[0]) {
    return 1;
  }
  for (int i = 1; i <= a[0]; i++) {
    if (tolower(a[i]) != tolower(b[i])) {
      return 1;
    }
  }
  return 0;
}
