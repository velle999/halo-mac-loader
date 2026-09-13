// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// The Resource Manager, read-only. A resource file is its resource fork,
// loaded whole when opened. Open files form a chain searched from the
// current file toward older ones, and a resource keeps one Handle while it
// is loaded, as on a Mac.

#define _GNU_SOURCE

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "carbon.h"

typedef struct res_file {
  SInt16 refnum;
  uint8_t* data;
  uint32_t length;
  char path[PATH_MAX];
  struct res_file* next;
} res_file;

typedef struct loaded_resource {
  Handle handle;
  res_file* file;
  ResType type;
  SInt16 id;
  struct loaded_resource* next;
} loaded_resource;

static pthread_mutex_t res_lock = PTHREAD_MUTEX_INITIALIZER;
// Most recently opened first.
static res_file* chain;
// 0 stands for the System file, which does not exist here; with it current,
// every open file is searched.
static SInt16 current_refnum;
// Distinct from fork reference numbers, which start at 100.
static SInt16 next_refnum = 2000;
static loaded_resource* loaded;
static __thread OSErr res_error;

int ResError(void) {
  return res_error;
}

static uint16_t be16(const uint8_t* p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}

// The fork header: data offset, map offset, data length, map length. The
// map repeats the header, then a handle, a file reference and attributes,
// then the offsets of the type list and name list.
#define MAP_TYPE_LIST_OFFSET 24
#define MIN_MAP_LENGTH 28

static int valid_fork(const uint8_t* d, uint32_t n) {
  if (n < 16) {
    return 0;
  }
  uint64_t data_end = (uint64_t)be32(d) + be32(d + 8);
  uint64_t map_offset = be32(d + 4);
  uint64_t map_end = map_offset + be32(d + 12);
  if (data_end > n || map_end > n || be32(d + 12) < MIN_MAP_LENGTH) {
    return 0;
  }
  uint64_t type_list = map_offset + be16(d + map_offset + MAP_TYPE_LIST_OFFSET);
  return type_list + 2 <= n;
}

// A resource's bytes in one file, or NULL.
static const uint8_t* find_in_file(const res_file* f, ResType type, SInt16 id,
                                   uint32_t* size) {
  const uint8_t* d = f->data;
  const uint8_t* end = d + f->length;
  const uint8_t* map = d + be32(d + 4);
  const uint8_t* type_list = map + be16(map + MAP_TYPE_LIST_OFFSET);
  // Both counts are stored minus one.
  int types = be16(type_list) + 1;
  for (int t = 0; t < types; t++) {
    const uint8_t* entry = type_list + 2 + t * 8;
    if (entry + 8 > end) {
      return NULL;
    }
    if (be32(entry) != type) {
      continue;
    }
    int count = be16(entry + 4) + 1;
    const uint8_t* refs = type_list + be16(entry + 6);
    for (int r = 0; r < count; r++) {
      const uint8_t* ref = refs + r * 12;
      if (ref + 12 > end) {
        return NULL;
      }
      if ((SInt16)be16(ref) != id) {
        continue;
      }
      const uint8_t* res = d + be32(d) + (be32(ref + 4) & 0x00FFFFFF);
      if (res + 4 > end || res + 4 + be32(res) > end) {
        return NULL;
      }
      *size = be32(res);
      return res + 4;
    }
  }
  return NULL;
}

static short open_resource_file(const char* path) {
  pthread_mutex_lock(&res_lock);
  for (res_file* f = chain; f; f = f->next) {
    if (!strcmp(f->path, path)) {
      current_refnum = f->refnum;
      pthread_mutex_unlock(&res_lock);
      res_error = noErr;
      return f->refnum;
    }
  }
  pthread_mutex_unlock(&res_lock);

  uint8_t* data;
  uint32_t length;
  OSErr err = hle_resource_fork_load(path, &data, &length);
  if (err) {
    cf_trace("FSOpenResFile(%s): no resource fork (%d)", path, err);
    res_error = err;
    return -1;
  }
  if (!valid_fork(data, length)) {
    free(data);
    res_error = mapReadErr;
    return -1;
  }
  res_file* f = calloc(1, sizeof(*f));
  f->data = data;
  f->length = length;
  snprintf(f->path, sizeof(f->path), "%s", path);
  pthread_mutex_lock(&res_lock);
  f->refnum = next_refnum++;
  f->next = chain;
  chain = f;
  current_refnum = f->refnum;
  pthread_mutex_unlock(&res_lock);
  cf_trace("FSOpenResFile(%s) = %d", path, f->refnum);
  res_error = noErr;
  return f->refnum;
}

short FSOpenResFile(const FSRef* ref, SInt8 permission) {
  char path[PATH_MAX];
  OSErr err = hle_ref_to_path(ref, path, sizeof(path));
  if (err) {
    res_error = err;
    return -1;
  }
  return open_resource_file(path);
}

short FSpOpenResFile(const FSSpec* spec, SInt8 permission) {
  char path[PATH_MAX];
  OSErr err = hle_spec_to_path(spec, path, sizeof(path));
  if (err) {
    res_error = err;
    return -1;
  }
  return open_resource_file(path);
}

void CloseResFile(SInt16 refnum) {
  pthread_mutex_lock(&res_lock);
  res_file** link = &chain;
  while (*link && (*link)->refnum != refnum) {
    link = &(*link)->next;
  }
  res_file* f = *link;
  if (!f) {
    pthread_mutex_unlock(&res_lock);
    res_error = resFNotFound;
    return;
  }
  *link = f->next;
  if (current_refnum == refnum) {
    current_refnum = chain ? chain->refnum : 0;
  }
  // Its resources go with it.
  loaded_resource** l = &loaded;
  loaded_resource* dropped = NULL;
  while (*l) {
    if ((*l)->file == f) {
      loaded_resource* gone = *l;
      *l = gone->next;
      gone->next = dropped;
      dropped = gone;
    } else {
      l = &(*l)->next;
    }
  }
  pthread_mutex_unlock(&res_lock);
  while (dropped) {
    loaded_resource* next = dropped->next;
    DisposeHandle(dropped->handle);
    free(dropped);
    dropped = next;
  }
  free(f->data);
  free(f);
  res_error = noErr;
}

short CurResFile(void) {
  res_error = noErr;
  return current_refnum;
}

void UseResFile(SInt16 refnum) {
  pthread_mutex_lock(&res_lock);
  res_file* f = chain;
  while (f && f->refnum != refnum) {
    f = f->next;
  }
  if (f || refnum == 0) {
    current_refnum = refnum;
    res_error = noErr;
  } else {
    res_error = resFNotFound;
  }
  pthread_mutex_unlock(&res_lock);
}

Handle GetResource(ResType type, SInt16 id) {
  pthread_mutex_lock(&res_lock);
  res_file* f = chain;
  if (current_refnum) {
    while (f && f->refnum != current_refnum) {
      f = f->next;
    }
  }
  for (; f; f = f->next) {
    for (loaded_resource* l = loaded; l; l = l->next) {
      if (l->file == f && l->type == type && l->id == id) {
        pthread_mutex_unlock(&res_lock);
        res_error = noErr;
        return l->handle;
      }
    }
    uint32_t size;
    const uint8_t* bytes = find_in_file(f, type, id, &size);
    if (bytes) {
      Handle h = hle_handle_new(bytes, size, 1);
      if (h) {
        loaded_resource* l = calloc(1, sizeof(*l));
        l->handle = h;
        l->file = f;
        l->type = type;
        l->id = id;
        l->next = loaded;
        loaded = l;
      }
      pthread_mutex_unlock(&res_lock);
      res_error = h ? noErr : memFullErr;
      return h;
    }
  }
  pthread_mutex_unlock(&res_lock);
  cf_trace("GetResource('%c%c%c%c', %d): not found", (int)(type >> 24) & 0xff,
           (int)(type >> 16) & 0xff, (int)(type >> 8) & 0xff,
           (int)type & 0xff, id);
  res_error = resNotFound;
  return NULL;
}

// Resources are read when their file opens, so this has nothing to do.
void LoadResource(Handle handle) {
  res_error = handle ? noErr : nilHandleErr;
}

// Takes |handle| off the loaded list; true when it was there.
static int forget(Handle handle) {
  pthread_mutex_lock(&res_lock);
  loaded_resource** l = &loaded;
  while (*l && (*l)->handle != handle) {
    l = &(*l)->next;
  }
  loaded_resource* found = *l;
  if (found) {
    *l = found->next;
  }
  pthread_mutex_unlock(&res_lock);
  free(found);
  return found != NULL;
}

void hle_resource_forget(Handle handle) {
  forget(handle);
}

void ReleaseResource(Handle handle) {
  if (!handle || !forget(handle)) {
    res_error = resNotFound;
    return;
  }
  DisposeHandle(handle);
  res_error = noErr;
}

// The handle stays valid and becomes the caller's.
void DetachResource(Handle handle) {
  res_error = handle && forget(handle) ? noErr : resNotFound;
}

// 'STR#': a big-endian count, then that many Pascal strings.
void GetIndString(unsigned char* out, SInt16 list_id, SInt16 index) {
  out[0] = 0;
  Handle h = GetResource('STR#', list_id);
  if (!h) {
    return;
  }
  const uint8_t* p = (const uint8_t*)*h;
  const uint8_t* end = p + GetHandleSize(h);
  if (end - p < 2 || index < 1 || index > be16(p)) {
    return;
  }
  const uint8_t* s = p + 2;
  for (int i = 1; i < index; i++) {
    if (s >= end) {
      return;
    }
    s += 1 + s[0];
  }
  if (s < end && s + 1 + s[0] <= end) {
    memcpy(out, s, 1 + s[0]);
  }
}

static void pascal_into(uint8_t* field, size_t size, const char* text) {
  size_t n = strlen(text);
  if (n > size - 1) {
    n = size - 1;
  }
  field[0] = (uint8_t)n;
  memcpy(field + 1, text, n);
}

// The System file's international resources, as a US English Mac has them:
// 'itl0', the formats of numbers, dates and times, and 'itl1', the names of
// days and months. Halo dates its time demo's results with them.
Handle GetIntlResource(SInt16 id) {
  static Handle itl0;
  static Handle itl1;
  if (id == 0) {
    if (!itl0) {
      static const uint8_t kIntl0[32] = {
        '.', ',', ';', '$', 0, 0,  // decimal point, separators, currency
        0xC0,  // currency with leading and trailing zeros
        0,  // dates in month, day, year order
        0x80,  // short dates with the century
        '/',
        255,  // a 12-hour clock
        0xC0,  // minutes and seconds with leading zeros
        'A', 'M', 0, 0,
        'P', 'M', 0, 0,
        ':',
        0, 0, 0, 0, 0, 0, 0, 0,  // no time suffixes
        0,  // not metric
        0, 0,  // version
      };
      itl0 = hle_handle_new(kIntl0, sizeof(kIntl0), 1);
    }
    res_error = itl0 ? noErr : memFullErr;
    return itl0;
  }
  if (id == 1) {
    if (!itl1) {
      static const char* const kDays[7] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
        "Saturday",
      };
      static const char* const kMonths[12] = {
        "January", "February", "March", "April", "May", "June", "July",
        "August", "September", "October", "November", "December",
      };
      uint8_t intl1[332];
      memset(intl1, 0, sizeof(intl1));
      for (int i = 0; i < 7; i++) {
        pascal_into(intl1 + 16 * i, 16, kDays[i]);
      }
      for (int i = 0; i < 12; i++) {
        pascal_into(intl1 + 112 + 16 * i, 16, kMonths[i]);
      }
      intl1[307] = 3;  // abbreviations of three letters
      memcpy(intl1 + 312, ", ", 2);  // after the day of the week
      memcpy(intl1 + 316, " ", 1);  // after the month
      memcpy(intl1 + 320, ", ", 2);  // after the day
      intl1[330] = 0x4E;  // no local routine: RTS
      intl1[331] = 0x75;
      itl1 = hle_handle_new(intl1, sizeof(intl1), 1);
    }
    res_error = itl1 ? noErr : memFullErr;
    return itl1;
  }
  char what[64];
  snprintf(what, sizeof(what), "GetIntlResource(%d)", id);
  fprintf(stderr, "hle: not implemented yet: %s\n", what);
  res_error = resNotFound;
  return NULL;
}
