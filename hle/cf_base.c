// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// The CF runtime -- retain, release, type IDs, descriptions -- and the small
// types: allocators, booleans, numbers, data, UUIDs, character sets, locales,
// number formatters and run loops.

#define _GNU_SOURCE

#include <locale.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#include "cf.h"

// Objects that are never freed: statics, interned UUIDs.
#define CF_IMMORTAL (-0x40000000)
#define CF_STATIC_BASE(cls) { { (cls), 0 }, CF_IMMORTAL }

// ---------------------------------------------------------------------------
// Buffers and diagnostics

void cf_buf_append(cf_buf* b, const char* s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t cap = b->cap ? b->cap : 64;
    while (b->len + n + 1 > cap) {
      cap *= 2;
    }
    b->data = realloc(b->data, cap);
    b->cap = cap;
  }
  memcpy(b->data + b->len, s, n);
  b->len += n;
  b->data[b->len] = '\0';
}

void cf_buf_appends(cf_buf* b, const char* s) {
  cf_buf_append(b, s, strlen(s));
}

void cf_buf_appendf(cf_buf* b, const char* fmt, ...) {
  char stack[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(stack, sizeof(stack), fmt, ap);
  va_end(ap);
  if (n < 0) {
    return;
  }
  if ((size_t)n < sizeof(stack)) {
    cf_buf_append(b, stack, n);
    return;
  }
  char* heap = malloc(n + 1);
  va_start(ap, fmt);
  vsnprintf(heap, n + 1, fmt, ap);
  va_end(ap);
  cf_buf_append(b, heap, n);
  free(heap);
}

void cf_buf_free(cf_buf* b) {
  free(b->data);
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
}

int cf_trace_enabled;

__attribute__((constructor)) static void cf_init_trace(void) {
  cf_trace_enabled = getenv("HLE_TRACE") != NULL;
}

void cf_trace(const char* fmt, ...) {
  if (!cf_trace_enabled) {
    return;
  }
  va_list ap;
  va_start(ap, fmt);
  fputs("[hle] ", stderr);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
}

void cf_warn_once(const char* what) {
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  static const char* seen[256];
  static int seen_count;
  pthread_mutex_lock(&lock);
  for (int i = 0; i < seen_count; i++) {
    if (!strcmp(seen[i], what)) {
      pthread_mutex_unlock(&lock);
      return;
    }
  }
  if (seen_count < 256) {
    seen[seen_count++] = what;
  }
  pthread_mutex_unlock(&lock);
  fprintf(stderr, "hle: not implemented yet: %s\n", what);
}

// ---------------------------------------------------------------------------
// Runtime

void* cf_alloc(const cf_class* cls, size_t size) {
  CFObject* obj = calloc(1, size);
  if (!obj) {
    fprintf(stderr, "hle: out of memory allocating a %s\n", cls->name);
    abort();
  }
  obj->base.isa = cls;
  obj->rc = 1;
  return obj;
}

const cf_class* cf_class_of(CFTypeRef obj) {
  const CFRuntimeBase* base = obj;
  if (base->isa == (const void*)__CFConstantStringClassReference) {
    return &cf_string_class;
  }
  return base->isa;
}

int cf_is(CFTypeRef obj, CFTypeID type_id) {
  return obj && cf_class_of(obj)->type_id == type_id;
}

int cf_equal(CFTypeRef a, CFTypeRef b) {
  if (a == b) {
    return 1;
  }
  if (!a || !b) {
    return 0;
  }
  const cf_class* cls = cf_class_of(a);
  if (cls != cf_class_of(b) || !cls->equal) {
    return 0;
  }
  return cls->equal(a, b);
}

CFHashCode cf_hash(CFTypeRef obj) {
  const cf_class* cls = cf_class_of(obj);
  return cls->hash ? cls->hash(obj) : (CFHashCode)obj;
}

void cf_describe(CFTypeRef obj, cf_buf* out) {
  if (!obj) {
    cf_buf_appends(out, "(null)");
    return;
  }
  const cf_class* cls = cf_class_of(obj);
  if (cls->describe) {
    cls->describe(obj, out);
  } else {
    cf_buf_appendf(out, "<%s %p>", cls->name, obj);
  }
}

static int is_constant_string(CFTypeRef obj) {
  return ((const CFRuntimeBase*)obj)->isa ==
         (const void*)__CFConstantStringClassReference;
}

CFTypeRef CFRetain(CFTypeRef obj) {
  if (!obj) {
    fprintf(stderr, "hle: CFRetain(NULL)\n");
    abort();
  }
  if (is_constant_string(obj)) {
    return obj;
  }
  CFObject* o = (CFObject*)obj;
  if (o->rc > 0) {
    __sync_add_and_fetch(&o->rc, 1);
  }
  return obj;
}

void CFRelease(CFTypeRef obj) {
  if (!obj) {
    fprintf(stderr, "hle: CFRelease(NULL)\n");
    abort();
  }
  if (is_constant_string(obj)) {
    return;
  }
  CFObject* o = (CFObject*)obj;
  if (o->rc <= 0) {
    return;
  }
  if (__sync_sub_and_fetch(&o->rc, 1) == 0) {
    const cf_class* cls = o->base.isa;
    if (cls->finalize) {
      cls->finalize(obj);
    }
    free(o);
  }
}

CFTypeID CFGetTypeID(CFTypeRef obj) {
  return cf_class_of(obj)->type_id;
}

int CFEqual(CFTypeRef a, CFTypeRef b) {
  return cf_equal(a, b);
}

CFHashCode CFHash(CFTypeRef obj) {
  return cf_hash(obj);
}

void CFShow(CFTypeRef obj) {
  cf_buf b = { 0 };
  cf_describe(obj, &b);
  fprintf(stderr, "%s\n", b.data ? b.data : "");
  cf_buf_free(&b);
}

// ---------------------------------------------------------------------------
// Allocators. kCFAllocatorDefault is NULL, as in CF; kCFAllocatorNull tells
// the NoCopy constructors not to free the caller's buffer.

struct __CFAllocator {
  CFObject obj;
  const char* name;
};

static void allocator_describe(CFTypeRef obj, cf_buf* out) {
  cf_buf_appends(out, ((const struct __CFAllocator*)obj)->name);
}

static const cf_class cf_allocator_class = {
  CF_TYPE_ALLOCATOR, "CFAllocator", NULL, NULL, NULL, allocator_describe,
};

static struct __CFAllocator system_allocator = {
  CF_STATIC_BASE(&cf_allocator_class), "kCFAllocatorSystemDefault",
};
static struct __CFAllocator malloc_allocator = {
  CF_STATIC_BASE(&cf_allocator_class), "kCFAllocatorMalloc",
};
static struct __CFAllocator null_allocator = {
  CF_STATIC_BASE(&cf_allocator_class), "kCFAllocatorNull",
};

const CFAllocatorRef kCFAllocatorDefault = NULL;
const CFAllocatorRef kCFAllocatorSystemDefault = &system_allocator;
const CFAllocatorRef kCFAllocatorMalloc = &malloc_allocator;
const CFAllocatorRef kCFAllocatorNull = &null_allocator;

CFTypeID CFAllocatorGetTypeID(void) {
  return CF_TYPE_ALLOCATOR;
}

// ---------------------------------------------------------------------------
// Booleans

struct __CFBoolean {
  CFObject obj;
  int value;
};

static void boolean_describe(CFTypeRef obj, cf_buf* out) {
  cf_buf_appends(out, ((const struct __CFBoolean*)obj)->value ? "true"
                                                              : "false");
}

static const cf_class cf_boolean_class = {
  CF_TYPE_BOOLEAN, "CFBoolean", NULL, NULL, NULL, boolean_describe,
};

static struct __CFBoolean boolean_true = { CF_STATIC_BASE(&cf_boolean_class), 1 };
static struct __CFBoolean boolean_false = { CF_STATIC_BASE(&cf_boolean_class), 0 };

const CFBooleanRef kCFBooleanTrue = &boolean_true;
const CFBooleanRef kCFBooleanFalse = &boolean_false;

CFTypeID CFBooleanGetTypeID(void) {
  return CF_TYPE_BOOLEAN;
}

int CFBooleanGetValue(CFBooleanRef boolean) {
  return boolean->value;
}

// ---------------------------------------------------------------------------
// Numbers

enum {
  kCFNumberSInt8Type = 1,
  kCFNumberSInt16Type = 2,
  kCFNumberSInt32Type = 3,
  kCFNumberSInt64Type = 4,
  kCFNumberFloat32Type = 5,
  kCFNumberFloat64Type = 6,
  kCFNumberCharType = 7,
  kCFNumberShortType = 8,
  kCFNumberIntType = 9,
  kCFNumberLongType = 10,
  kCFNumberLongLongType = 11,
  kCFNumberFloatType = 12,
  kCFNumberDoubleType = 13,
  kCFNumberCFIndexType = 14,
  kCFNumberNSIntegerType = 15,
  kCFNumberCGFloatType = 16,
};

struct __CFNumber {
  CFObject obj;
  CFIndex type;
  int is_float;
  int64_t i;
  double d;
};

// Sizes on i386, where long, CFIndex, NSInteger and CGFloat are 4 bytes.
static CFIndex number_type_size(CFIndex type) {
  switch (type) {
    case kCFNumberSInt8Type:
    case kCFNumberCharType:
      return 1;
    case kCFNumberSInt16Type:
    case kCFNumberShortType:
      return 2;
    case kCFNumberSInt64Type:
    case kCFNumberLongLongType:
    case kCFNumberFloat64Type:
    case kCFNumberDoubleType:
      return 8;
    default:
      return 4;
  }
}

static int number_type_is_float(CFIndex type) {
  return type == kCFNumberFloat32Type || type == kCFNumberFloat64Type ||
         type == kCFNumberFloatType || type == kCFNumberDoubleType ||
         type == kCFNumberCGFloatType;
}

static int number_equal(CFTypeRef a, CFTypeRef b) {
  const struct __CFNumber* x = a;
  const struct __CFNumber* y = b;
  if (x->is_float || y->is_float) {
    return cf_number_as_double(a) == cf_number_as_double(b);
  }
  return x->i == y->i;
}

static CFHashCode number_hash(CFTypeRef obj) {
  const struct __CFNumber* n = obj;
  return n->is_float ? (CFHashCode)(int64_t)n->d : (CFHashCode)n->i;
}

static void number_describe(CFTypeRef obj, cf_buf* out) {
  const struct __CFNumber* n = obj;
  if (n->is_float) {
    cf_buf_appendf(out, "%g", n->d);
  } else {
    cf_buf_appendf(out, "%lld", (long long)n->i);
  }
}

static const cf_class cf_number_class = {
  CF_TYPE_NUMBER, "CFNumber", NULL, number_equal, number_hash, number_describe,
};

CFTypeID CFNumberGetTypeID(void) {
  return CF_TYPE_NUMBER;
}

CFNumberRef CFNumberCreate(CFAllocatorRef allocator, CFIndex type,
                           const void* value) {
  struct __CFNumber* n = cf_alloc(&cf_number_class, sizeof(*n));
  n->type = type;
  n->is_float = number_type_is_float(type);
  switch (number_type_size(type)) {
    case 1:
      n->i = *(const int8_t*)value;
      break;
    case 2:
      n->i = *(const int16_t*)value;
      break;
    case 4:
      if (n->is_float) {
        n->d = *(const float*)value;
      } else {
        n->i = *(const int32_t*)value;
      }
      break;
    case 8:
      if (n->is_float) {
        n->d = *(const double*)value;
      } else {
        n->i = *(const int64_t*)value;
      }
      break;
  }
  return n;
}

CFIndex CFNumberGetByteSize(CFNumberRef number) {
  return number_type_size(number->type);
}

// True when the value fits the requested type exactly.
int CFNumberGetValue(CFNumberRef number, CFIndex type, void* value) {
  if (number_type_is_float(type)) {
    double d = cf_number_as_double(number);
    if (number_type_size(type) == 8) {
      *(double*)value = d;
      return 1;
    }
    *(float*)value = (float)d;
    return (double)(float)d == d;
  }
  int64_t i = cf_number_as_int(number);
  int exact = !number->is_float || (double)i == number->d;
  switch (number_type_size(type)) {
    case 1:
      *(int8_t*)value = (int8_t)i;
      return exact && i == (int8_t)i;
    case 2:
      *(int16_t*)value = (int16_t)i;
      return exact && i == (int16_t)i;
    case 4:
      *(int32_t*)value = (int32_t)i;
      return exact && i == (int32_t)i;
    default:
      *(int64_t*)value = i;
      return exact;
  }
}

CFNumberRef cf_number_int(int64_t value) {
  if (value == (int32_t)value) {
    int32_t v = (int32_t)value;
    return CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
  }
  return CFNumberCreate(NULL, kCFNumberSInt64Type, &value);
}

CFNumberRef cf_number_double(double value) {
  return CFNumberCreate(NULL, kCFNumberFloat64Type, &value);
}

int cf_number_is_float(CFNumberRef number) {
  return number->is_float;
}

int64_t cf_number_as_int(CFNumberRef number) {
  return number->is_float ? (int64_t)number->d : number->i;
}

double cf_number_as_double(CFNumberRef number) {
  return number->is_float ? number->d : (double)number->i;
}

// ---------------------------------------------------------------------------
// Data

struct __CFData {
  CFObject obj;
  UInt8* bytes;
  CFIndex length;
};

static void data_finalize(CFTypeRef obj) {
  free(((struct __CFData*)obj)->bytes);
}

static int data_equal(CFTypeRef a, CFTypeRef b) {
  const struct __CFData* x = a;
  const struct __CFData* y = b;
  return x->length == y->length && !memcmp(x->bytes, y->bytes, x->length);
}

static void data_describe(CFTypeRef obj, cf_buf* out) {
  cf_buf_appendf(out, "<CFData length %ld>",
                 (long)((const struct __CFData*)obj)->length);
}

static const cf_class cf_data_class = {
  CF_TYPE_DATA, "CFData", data_finalize, data_equal, NULL, data_describe,
};

CFTypeID CFDataGetTypeID(void) {
  return CF_TYPE_DATA;
}

CFDataRef cf_data_create(const UInt8* bytes, CFIndex length) {
  struct __CFData* d = cf_alloc(&cf_data_class, sizeof(*d));
  d->bytes = malloc(length > 0 ? length : 1);
  if (length > 0) {
    memcpy(d->bytes, bytes, length);
  }
  d->length = length;
  return d;
}

CFDataRef CFDataCreate(CFAllocatorRef allocator, const UInt8* bytes,
                       CFIndex length) {
  return cf_data_create(bytes, length);
}

const UInt8* CFDataGetBytePtr(CFDataRef data) {
  return data->bytes;
}

CFIndex CFDataGetLength(CFDataRef data) {
  return data->length;
}

void CFDataGetBytes(CFDataRef data, CFRange range, UInt8* buffer) {
  memcpy(buffer, data->bytes + range.location, range.length);
}

const UInt8* cf_data_bytes(CFDataRef data) {
  return data->bytes;
}

CFIndex cf_data_length(CFDataRef data) {
  return data->length;
}

// ---------------------------------------------------------------------------
// UUIDs. The game builds IOKit's plug-in interface IDs with
// CFUUIDGetConstantUUIDWithBytes, which interns: the same bytes give the
// same object.

struct __CFUUID {
  CFObject obj;
  CFUUIDBytes bytes;
  struct __CFUUID* next;
};

static void uuid_describe(CFTypeRef obj, cf_buf* out) {
  const UInt8* b = ((const struct __CFUUID*)obj)->bytes.bytes;
  cf_buf_appendf(out, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-"
                 "%02X%02X%02X%02X%02X%02X",
                 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9],
                 b[10], b[11], b[12], b[13], b[14], b[15]);
}

static const cf_class cf_uuid_class = {
  CF_TYPE_UUID, "CFUUID", NULL, NULL, NULL, uuid_describe,
};

CFTypeID CFUUIDGetTypeID(void) {
  return CF_TYPE_UUID;
}

// The sixteen bytes arrive as sixteen stack slots.
CFUUIDRef CFUUIDGetConstantUUIDWithBytes(
    CFAllocatorRef allocator,
    unsigned int b0, unsigned int b1, unsigned int b2, unsigned int b3,
    unsigned int b4, unsigned int b5, unsigned int b6, unsigned int b7,
    unsigned int b8, unsigned int b9, unsigned int b10, unsigned int b11,
    unsigned int b12, unsigned int b13, unsigned int b14, unsigned int b15) {
  static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
  static struct __CFUUID* interned;
  const unsigned int in[16] = { b0, b1, b2, b3, b4, b5, b6, b7,
                                b8, b9, b10, b11, b12, b13, b14, b15 };
  CFUUIDBytes bytes;
  for (int i = 0; i < 16; i++) {
    bytes.bytes[i] = in[i] & 0xff;
  }
  pthread_mutex_lock(&lock);
  struct __CFUUID* u;
  for (u = interned; u; u = u->next) {
    if (!memcmp(u->bytes.bytes, bytes.bytes, 16)) {
      break;
    }
  }
  if (!u) {
    u = cf_alloc(&cf_uuid_class, sizeof(*u));
    u->obj.rc = CF_IMMORTAL;
    u->bytes = bytes;
    u->next = interned;
    interned = u;
  }
  pthread_mutex_unlock(&lock);
  return u;
}

// CFUUIDBytes is returned in memory; see cf.h for why the pointer is explicit.
CFUUIDBytes* CFUUIDGetUUIDBytes(CFUUIDBytes* result, CFUUIDRef uuid) {
  *result = uuid->bytes;
  return result;
}

// ---------------------------------------------------------------------------
// Character sets: the predefined ones, classified with the C.UTF-8 locale.

enum {
  kCFCharacterSetControl = 1,
  kCFCharacterSetWhitespace,
  kCFCharacterSetWhitespaceAndNewline,
  kCFCharacterSetDecimalDigit,
  kCFCharacterSetLetter,
  kCFCharacterSetLowercaseLetter,
  kCFCharacterSetUppercaseLetter,
  kCFCharacterSetNonBase,
  kCFCharacterSetDecomposable,
  kCFCharacterSetAlphaNumeric,
  kCFCharacterSetPunctuation,
  kCFCharacterSetIllegal,
  kCFCharacterSetCapitalizedLetter,
  kCFCharacterSetSymbol,
  kCFCharacterSetNewline,
  kCFCharacterSetCount,
};

struct __CFCharacterSet {
  CFObject obj;
  CFIndex identifier;
};

static const cf_class cf_character_set_class = {
  CF_TYPE_CHARACTER_SET, "CFCharacterSet", NULL, NULL, NULL, NULL,
};

static struct __CFCharacterSet predefined_sets[kCFCharacterSetCount];
static locale_t utf8_locale;

static pthread_once_t character_sets_once = PTHREAD_ONCE_INIT;

static void init_character_sets(void) {
  for (int i = 0; i < kCFCharacterSetCount; i++) {
    predefined_sets[i].obj.base.isa = &cf_character_set_class;
    predefined_sets[i].obj.rc = CF_IMMORTAL;
    predefined_sets[i].identifier = i;
  }
  utf8_locale = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
}

CFTypeID CFCharacterSetGetTypeID(void) {
  return CF_TYPE_CHARACTER_SET;
}

CFCharacterSetRef CFCharacterSetGetPredefined(CFIndex identifier) {
  pthread_once(&character_sets_once, init_character_sets);
  if (identifier <= 0 || identifier >= kCFCharacterSetCount) {
    return NULL;
  }
  return &predefined_sets[identifier];
}

static int is_newline(wint_t c) {
  return c == '\n' || c == '\r' || c == 0x85 || c == 0x2028 || c == 0x2029;
}

int CFCharacterSetIsCharacterMember(CFCharacterSetRef set, unsigned int ch) {
  wint_t c = ch & 0xffff;
  locale_t loc = utf8_locale ? utf8_locale : uselocale((locale_t)0);
  switch (set->identifier) {
    case kCFCharacterSetControl:
      return iswcntrl_l(c, loc) != 0;
    case kCFCharacterSetWhitespace:
      return c == ' ' || c == '\t' || (iswblank_l(c, loc) && !is_newline(c));
    case kCFCharacterSetWhitespaceAndNewline:
      return iswspace_l(c, loc) || is_newline(c);
    case kCFCharacterSetNewline:
      return is_newline(c);
    case kCFCharacterSetDecimalDigit:
      return iswdigit_l(c, loc) != 0;
    case kCFCharacterSetLetter:
      return iswalpha_l(c, loc) != 0;
    case kCFCharacterSetLowercaseLetter:
      return iswlower_l(c, loc) != 0;
    case kCFCharacterSetUppercaseLetter:
    case kCFCharacterSetCapitalizedLetter:
      return iswupper_l(c, loc) != 0;
    case kCFCharacterSetAlphaNumeric:
      return iswalnum_l(c, loc) != 0;
    case kCFCharacterSetPunctuation:
    case kCFCharacterSetSymbol:
      return iswpunct_l(c, loc) != 0;
    default:
      cf_warn_once("CFCharacterSetIsCharacterMember for this predefined set");
      return 0;
  }
}

// ---------------------------------------------------------------------------
// Locales and number formatters: objects to hold, nothing localized yet.

struct __CFLocale {
  CFObject obj;
};

static void locale_describe(CFTypeRef obj, cf_buf* out) {
  cf_buf_appends(out, "en_US");
}

static const cf_class cf_locale_class = {
  CF_TYPE_LOCALE, "CFLocale", NULL, NULL, NULL, locale_describe,
};

static struct __CFLocale current_locale = { CF_STATIC_BASE(&cf_locale_class) };

CFTypeID CFLocaleGetTypeID(void) {
  return CF_TYPE_LOCALE;
}

CFLocaleRef CFLocaleCopyCurrent(void) {
  return &current_locale;
}

struct __CFNumberFormatter {
  CFObject obj;
  CFLocaleRef locale;
  CFIndex style;
};

static const cf_class cf_number_formatter_class = {
  CF_TYPE_NUMBER_FORMATTER, "CFNumberFormatter", NULL, NULL, NULL, NULL,
};

CFTypeID CFNumberFormatterGetTypeID(void) {
  return CF_TYPE_NUMBER_FORMATTER;
}

CFNumberFormatterRef CFNumberFormatterCreate(CFAllocatorRef allocator,
                                             CFLocaleRef locale,
                                             CFIndex style) {
  struct __CFNumberFormatter* f =
      cf_alloc(&cf_number_formatter_class, sizeof(*f));
  f->locale = locale;
  f->style = style;
  return f;
}

// ---------------------------------------------------------------------------
// Run loops. One loop stands for every thread for now; sources are kept so
// the event loop can service IOKit's notification ports later.

#define RUN_LOOP_MAX_SOURCES 64

struct __CFRunLoop {
  CFObject obj;
  pthread_mutex_t lock;
  CFRunLoopSourceRef sources[RUN_LOOP_MAX_SOURCES];
  int count;
};

static const cf_class cf_run_loop_class = {
  CF_TYPE_RUN_LOOP, "CFRunLoop", NULL, NULL, NULL, NULL,
};

static struct __CFRunLoop main_run_loop = {
  CF_STATIC_BASE(&cf_run_loop_class), PTHREAD_MUTEX_INITIALIZER, { 0 }, 0,
};

CFTypeID CFRunLoopGetTypeID(void) {
  return CF_TYPE_RUN_LOOP;
}

CFRunLoopRef CFRunLoopGetCurrent(void) {
  return &main_run_loop;
}

CFRunLoopRef CFRunLoopGetMain(void) {
  return &main_run_loop;
}

static int run_loop_index(CFRunLoopRef loop, CFRunLoopSourceRef source) {
  for (int i = 0; i < loop->count; i++) {
    if (loop->sources[i] == source) {
      return i;
    }
  }
  return -1;
}

void CFRunLoopAddSource(CFRunLoopRef loop, CFRunLoopSourceRef source,
                        CFStringRef mode) {
  pthread_mutex_lock(&loop->lock);
  if (run_loop_index(loop, source) < 0) {
    if (loop->count < RUN_LOOP_MAX_SOURCES) {
      loop->sources[loop->count++] = source;
      CFRetain(source);
    } else {
      cf_warn_once("more than 64 run loop sources");
    }
  }
  pthread_mutex_unlock(&loop->lock);
}

void CFRunLoopRemoveSource(CFRunLoopRef loop, CFRunLoopSourceRef source,
                           CFStringRef mode) {
  pthread_mutex_lock(&loop->lock);
  int i = run_loop_index(loop, source);
  if (i >= 0) {
    loop->sources[i] = loop->sources[--loop->count];
  }
  pthread_mutex_unlock(&loop->lock);
  if (i >= 0) {
    CFRelease(source);
  }
}

int CFRunLoopContainsSource(CFRunLoopRef loop, CFRunLoopSourceRef source,
                            CFStringRef mode) {
  pthread_mutex_lock(&loop->lock);
  int found = run_loop_index(loop, source) >= 0;
  pthread_mutex_unlock(&loop->lock);
  return found;
}
