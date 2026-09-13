// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// CFString. Allocated strings hold UTF-16 code units. Constant strings stay
// in the image as the compiler emitted them (8-bit ASCII, or UTF-16 when the
// literal was not ASCII) and are read in place.

#define _GNU_SOURCE

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#include "cf.h"

extern const CFAllocatorRef kCFAllocatorNull;

// The address every constant string's isa holds. Its contents are unused.
int __CFConstantStringClassReference[12];

typedef struct {
  CFRuntimeBase base;
  const char* bytes;
  long length;
} cf_constant_string;

// Constant strings with non-ASCII literals are emitted as UTF-16.
#define CF_CONSTANT_UTF16_INFO 0x07d0

#define CF_STRING_MUTABLE 1
// chars belong to the caller (CFStringCreateMutableWithExternalCharactersNoCopy)
#define CF_STRING_EXTERNAL 2

struct __CFString {
  CFObject obj;
  unsigned flags;
  UniChar* chars;
  CFIndex length;
  CFIndex capacity;
};

#define CF_CONSTANT_STRING(name, text)                                   \
  static const cf_constant_string name##_storage = {                     \
    { __CFConstantStringClassReference, 0x07c8 }, text, sizeof(text) - 1 \
  };                                                                     \
  const CFStringRef name = (CFStringRef)&name##_storage

CF_CONSTANT_STRING(kCFRunLoopDefaultMode, "kCFRunLoopDefaultMode");
CF_CONSTANT_STRING(kCFRunLoopCommonModes, "kCFRunLoopCommonModes");
CF_CONSTANT_STRING(kCFPreferencesCurrentApplication,
                   "kCFPreferencesCurrentApplication");
CF_CONSTANT_STRING(kCFPreferencesAnyApplication,
                   "kCFPreferencesAnyApplication");
CF_CONSTANT_STRING(kCFPreferencesCurrentUser, "kCFPreferencesCurrentUser");
CF_CONSTANT_STRING(kCFPreferencesAnyUser, "kCFPreferencesAnyUser");
CF_CONSTANT_STRING(kCFPreferencesCurrentHost, "kCFPreferencesCurrentHost");
CF_CONSTANT_STRING(kCFPreferencesAnyHost, "kCFPreferencesAnyHost");

static int is_constant(CFStringRef s) {
  return s->obj.base.isa == (const void*)__CFConstantStringClassReference;
}

CFIndex cf_string_length(CFStringRef s) {
  if (is_constant(s)) {
    return ((const cf_constant_string*)s)->length;
  }
  return s->length;
}

UniChar cf_string_char(CFStringRef s, CFIndex i) {
  if (is_constant(s)) {
    const cf_constant_string* c = (const cf_constant_string*)s;
    if (c->base.info == CF_CONSTANT_UTF16_INFO) {
      return ((const UniChar*)c->bytes)[i];
    }
    return (unsigned char)c->bytes[i];
  }
  return s->chars[i];
}

// ---------------------------------------------------------------------------
// Encodings

// Mac OS Roman, 0x80-0xFF.
static const UniChar kMacRoman[128] = {
  0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1,
  0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
  0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
  0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
  0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF,
  0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
  0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
  0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
  0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
  0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
  0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA,
  0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
  0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
  0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
  0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
  0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

// Windows-1252, 0x80-0x9F. Zero marks the five unassigned bytes.
static const UniChar kWindowsLatin1[32] = {
  0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
  0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
  0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
  0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178,
};

static int is_utf16_encoding(CFStringEncoding encoding) {
  return encoding == kCFStringEncodingUnicode ||
         encoding == kCFStringEncodingUTF16BE ||
         encoding == kCFStringEncodingUTF16LE;
}

static int single_byte_encoding(CFStringEncoding encoding) {
  switch (encoding) {
    case kCFStringEncodingMacRoman:
    case kCFStringEncodingISOLatin1:
    case kCFStringEncodingWindowsLatin1:
    case kCFStringEncodingASCII:
    case kCFStringEncodingNextStepLatin:
    case kCFStringEncodingNonLossyASCII:
      return 1;
    default:
      return 0;
  }
}

static UniChar decode_single_byte(unsigned char b, CFStringEncoding encoding) {
  if (b < 0x80) {
    return b;
  }
  if (encoding == kCFStringEncodingMacRoman) {
    return kMacRoman[b - 0x80];
  }
  if (encoding == kCFStringEncodingWindowsLatin1 && b < 0xA0 &&
      kWindowsLatin1[b - 0x80]) {
    return kWindowsLatin1[b - 0x80];
  }
  return b;
}

// The byte for |c| in a single-byte encoding, or -1 when it has none.
static int encode_single_byte(UniChar c, CFStringEncoding encoding) {
  if (c < 0x80) {
    return c;
  }
  if (encoding == kCFStringEncodingASCII ||
      encoding == kCFStringEncodingNonLossyASCII) {
    return -1;
  }
  if (encoding == kCFStringEncodingMacRoman) {
    for (int i = 0; i < 128; i++) {
      if (kMacRoman[i] == c) {
        return 0x80 + i;
      }
    }
    return -1;
  }
  if (encoding == kCFStringEncodingWindowsLatin1) {
    for (int i = 0; i < 32; i++) {
      if (kWindowsLatin1[i] == c) {
        return 0x80 + i;
      }
    }
    if (c >= 0x80 && c < 0xA0) {
      return -1;
    }
  }
  return c < 0x100 ? c : -1;
}

// Decodes |n| bytes into a malloc'd UTF-16 array. NULL for malformed input
// or an encoding this file does not know.
static UniChar* decode(const UInt8* bytes, CFIndex n, CFStringEncoding encoding,
                       CFIndex* out_length) {
  UniChar* out = malloc(sizeof(UniChar) * (n > 0 ? n : 1));
  CFIndex len = 0;
  if (single_byte_encoding(encoding)) {
    for (CFIndex i = 0; i < n; i++) {
      out[len++] = decode_single_byte(bytes[i], encoding);
    }
  } else if (encoding == kCFStringEncodingUTF8) {
    CFIndex i = 0;
    if (n >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
      i = 3;
    }
    while (i < n) {
      uint32_t c = bytes[i];
      int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1
                : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
      if (extra < 0 || i + extra >= n) {
        free(out);
        return NULL;
      }
      if (extra > 0) {
        c &= 0x3F >> extra;
        for (int k = 1; k <= extra; k++) {
          if ((bytes[i + k] & 0xC0) != 0x80) {
            free(out);
            return NULL;
          }
          c = (c << 6) | (bytes[i + k] & 0x3F);
        }
      }
      i += extra + 1;
      if (c >= 0x10000) {
        c -= 0x10000;
        out[len++] = 0xD800 + (c >> 10);
        out[len++] = 0xDC00 + (c & 0x3FF);
      } else {
        out[len++] = c;
      }
    }
  } else if (is_utf16_encoding(encoding)) {
    int big_endian = encoding == kCFStringEncodingUTF16BE;
    CFIndex i = 0;
    if (n >= 2 && encoding == kCFStringEncodingUnicode) {
      if (bytes[0] == 0xFE && bytes[1] == 0xFF) {
        big_endian = 1;
        i = 2;
      } else if (bytes[0] == 0xFF && bytes[1] == 0xFE) {
        i = 2;
      }
    }
    for (; i + 1 < n; i += 2) {
      out[len++] = big_endian ? (bytes[i] << 8) | bytes[i + 1]
                              : bytes[i] | (bytes[i + 1] << 8);
    }
  } else {
    cf_warn_once("decoding a string encoding other than MacRoman, Latin-1, "
                 "ASCII, UTF-8 or UTF-16");
    free(out);
    return NULL;
  }
  *out_length = len;
  return out;
}

// ---------------------------------------------------------------------------
// The class

static void string_finalize(CFTypeRef obj) {
  struct __CFString* s = (struct __CFString*)obj;
  if (!(s->flags & CF_STRING_EXTERNAL)) {
    free(s->chars);
  }
}

static int string_equal(CFTypeRef a, CFTypeRef b) {
  CFIndex n = cf_string_length(a);
  if (n != cf_string_length(b)) {
    return 0;
  }
  for (CFIndex i = 0; i < n; i++) {
    if (cf_string_char(a, i) != cf_string_char(b, i)) {
      return 0;
    }
  }
  return 1;
}

static CFHashCode string_hash(CFTypeRef obj) {
  uint32_t h = 2166136261u;
  CFIndex n = cf_string_length(obj);
  for (CFIndex i = 0; i < n; i++) {
    h = (h ^ cf_string_char(obj, i)) * 16777619u;
  }
  return h;
}

static void string_describe(CFTypeRef obj, cf_buf* out) {
  char* utf8 = cf_string_utf8(obj);
  cf_buf_appends(out, utf8);
  free(utf8);
}

const cf_class cf_string_class = {
  CF_TYPE_STRING, "CFString", string_finalize, string_equal, string_hash,
  string_describe,
};

static struct __CFString* string_new(UniChar* chars, CFIndex length,
                                     unsigned flags) {
  struct __CFString* s = cf_alloc(&cf_string_class, sizeof(*s));
  s->chars = chars;
  s->length = length;
  s->capacity = length;
  s->flags = flags;
  return s;
}

static void string_reserve(struct __CFString* s, CFIndex needed) {
  if (needed <= s->capacity && s->chars) {
    return;
  }
  CFIndex capacity = s->capacity > 16 ? s->capacity : 16;
  while (capacity < needed) {
    capacity *= 2;
  }
  UniChar* chars = malloc(sizeof(UniChar) * capacity);
  if (s->length) {
    memcpy(chars, s->chars, sizeof(UniChar) * s->length);
  }
  if (!(s->flags & CF_STRING_EXTERNAL)) {
    free(s->chars);
  }
  s->flags &= ~CF_STRING_EXTERNAL;
  s->chars = chars;
  s->capacity = capacity;
}

static void string_replace(struct __CFString* s, CFRange range,
                           const UniChar* chars, CFIndex n) {
  CFIndex new_length = s->length - range.length + n;
  string_reserve(s, new_length);
  memmove(s->chars + range.location + n,
          s->chars + range.location + range.length,
          sizeof(UniChar) * (s->length - range.location - range.length));
  if (n) {
    memcpy(s->chars + range.location, chars, sizeof(UniChar) * n);
  }
  s->length = new_length;
}

static UniChar* copy_chars(CFStringRef s, CFIndex* length) {
  CFIndex n = cf_string_length(s);
  UniChar* chars = malloc(sizeof(UniChar) * (n > 0 ? n : 1));
  for (CFIndex i = 0; i < n; i++) {
    chars[i] = cf_string_char(s, i);
  }
  *length = n;
  return chars;
}

char* cf_string_utf8(CFStringRef s) {
  CFIndex n = cf_string_length(s);
  char* out = malloc(3 * n + 1);
  size_t o = 0;
  for (CFIndex i = 0; i < n; i++) {
    uint32_t c = cf_string_char(s, i);
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
      UniChar low = cf_string_char(s, i + 1);
      if (low >= 0xDC00 && low < 0xE000) {
        c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
        i++;
      }
    }
    if (c < 0x80) {
      out[o++] = c;
    } else if (c < 0x800) {
      out[o++] = 0xC0 | (c >> 6);
      out[o++] = 0x80 | (c & 0x3F);
    } else if (c < 0x10000) {
      out[o++] = 0xE0 | (c >> 12);
      out[o++] = 0x80 | ((c >> 6) & 0x3F);
      out[o++] = 0x80 | (c & 0x3F);
    } else {
      out[o++] = 0xF0 | (c >> 18);
      out[o++] = 0x80 | ((c >> 12) & 0x3F);
      out[o++] = 0x80 | ((c >> 6) & 0x3F);
      out[o++] = 0x80 | (c & 0x3F);
    }
  }
  out[o] = '\0';
  return out;
}

CFStringRef cf_string_from_utf8(const char* utf8, size_t n) {
  CFIndex length;
  UniChar* chars = decode((const UInt8*)utf8, n, kCFStringEncodingUTF8, &length);
  if (!chars) {
    chars = decode((const UInt8*)utf8, n, kCFStringEncodingMacRoman, &length);
  }
  return string_new(chars, length, 0);
}

int cf_string_is_ascii(CFStringRef s, const char* ascii) {
  CFIndex n = cf_string_length(s);
  for (CFIndex i = 0; i < n; i++) {
    if (!ascii[i] || cf_string_char(s, i) != (unsigned char)ascii[i]) {
      return 0;
    }
  }
  return ascii[n] == '\0';
}

// ---------------------------------------------------------------------------
// Creation

CFTypeID CFStringGetTypeID(void) {
  return CF_TYPE_STRING;
}

CFStringRef CFStringCreateWithCString(CFAllocatorRef allocator, const char* str,
                                      CFStringEncoding encoding) {
  CFIndex length;
  UniChar* chars = decode((const UInt8*)str, strlen(str), encoding, &length);
  return chars ? string_new(chars, length, 0) : NULL;
}

// Copies, then frees the caller's buffer the way CF would have later.
CFStringRef CFStringCreateWithCStringNoCopy(CFAllocatorRef allocator,
                                            const char* str,
                                            CFStringEncoding encoding,
                                            CFAllocatorRef deallocator) {
  CFStringRef s = CFStringCreateWithCString(allocator, str, encoding);
  if (s && deallocator != kCFAllocatorNull) {
    free((char*)str);
  }
  return s;
}

CFStringRef CFStringCreateWithCharacters(CFAllocatorRef allocator,
                                         const UniChar* chars,
                                         CFIndex length) {
  UniChar* copy = malloc(sizeof(UniChar) * (length > 0 ? length : 1));
  if (length > 0) {
    memcpy(copy, chars, sizeof(UniChar) * length);
  }
  return string_new(copy, length, 0);
}

CFStringRef CFStringCreateWithCharactersNoCopy(CFAllocatorRef allocator,
                                               const UniChar* chars,
                                               CFIndex length,
                                               CFAllocatorRef deallocator) {
  CFStringRef s = CFStringCreateWithCharacters(allocator, chars, length);
  if (deallocator != kCFAllocatorNull) {
    free((UniChar*)chars);
  }
  return s;
}

CFMutableStringRef CFStringCreateMutableCopy(CFAllocatorRef allocator,
                                             CFIndex max_length,
                                             CFStringRef str) {
  CFIndex length;
  UniChar* chars = copy_chars(str, &length);
  return string_new(chars, length, CF_STRING_MUTABLE);
}

CFMutableStringRef CFStringCreateMutable(CFAllocatorRef allocator,
                                         CFIndex max_length) {
  return string_new(NULL, 0, CF_STRING_MUTABLE);
}

// Mutations happen in the caller's buffer until they outgrow |capacity|.
CFMutableStringRef CFStringCreateMutableWithExternalCharactersNoCopy(
    CFAllocatorRef allocator, UniChar* chars, CFIndex length, CFIndex capacity,
    CFAllocatorRef external_allocator) {
  struct __CFString* s =
      string_new(chars, length, CF_STRING_MUTABLE | CF_STRING_EXTERNAL);
  s->capacity = capacity;
  return s;
}

// ---------------------------------------------------------------------------
// Reading

CFIndex CFStringGetLength(CFStringRef str) {
  return cf_string_length(str);
}

unsigned int CFStringGetCharacterAtIndex(CFStringRef str, CFIndex index) {
  return cf_string_char(str, index);
}

void CFStringGetCharacters(CFStringRef str, CFRange range, UniChar* buffer) {
  for (CFIndex i = 0; i < range.length; i++) {
    buffer[i] = cf_string_char(str, range.location + i);
  }
}

CFStringEncoding CFStringGetSystemEncoding(void) {
  return kCFStringEncodingMacRoman;
}

CFIndex CFStringGetBytes(CFStringRef str, CFRange range,
                         CFStringEncoding encoding, unsigned int loss_byte,
                         unsigned int external_representation, UInt8* buffer,
                         CFIndex max_length, CFIndex* used_length) {
  CFIndex used = 0;
  CFIndex converted = 0;
  loss_byte &= 0xff;
  external_representation &= 0xff;
  if (is_utf16_encoding(encoding)) {
    int big_endian = encoding == kCFStringEncodingUTF16BE;
    if (external_representation && encoding == kCFStringEncodingUnicode) {
      if (buffer) {
        if (max_length < 2) {
          goto done;
        }
        buffer[0] = 0xFF;
        buffer[1] = 0xFE;
      }
      used = 2;
    }
    for (; converted < range.length; converted++) {
      if (buffer && used + 2 > max_length) {
        break;
      }
      UniChar c = cf_string_char(str, range.location + converted);
      if (buffer) {
        buffer[used + !big_endian] = c >> 8;
        buffer[used + big_endian] = c & 0xff;
      }
      used += 2;
    }
  } else if (encoding == kCFStringEncodingUTF8) {
    for (; converted < range.length; converted++) {
      uint32_t c = cf_string_char(str, range.location + converted);
      UInt8 tmp[4];
      int n;
      int pair = 0;
      if (c >= 0xD800 && c < 0xDC00 && converted + 1 < range.length) {
        UniChar low = cf_string_char(str, range.location + converted + 1);
        if (low >= 0xDC00 && low < 0xE000) {
          c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
          pair = 1;
        }
      }
      if (c < 0x80) {
        tmp[0] = c;
        n = 1;
      } else if (c < 0x800) {
        tmp[0] = 0xC0 | (c >> 6);
        tmp[1] = 0x80 | (c & 0x3F);
        n = 2;
      } else if (c < 0x10000) {
        tmp[0] = 0xE0 | (c >> 12);
        tmp[1] = 0x80 | ((c >> 6) & 0x3F);
        tmp[2] = 0x80 | (c & 0x3F);
        n = 3;
      } else {
        tmp[0] = 0xF0 | (c >> 18);
        tmp[1] = 0x80 | ((c >> 12) & 0x3F);
        tmp[2] = 0x80 | ((c >> 6) & 0x3F);
        tmp[3] = 0x80 | (c & 0x3F);
        n = 4;
      }
      if (buffer && used + n > max_length) {
        break;
      }
      if (buffer) {
        memcpy(buffer + used, tmp, n);
      }
      used += n;
      converted += pair;
    }
  } else {
    for (; converted < range.length; converted++) {
      if (buffer && used + 1 > max_length) {
        break;
      }
      int b = encode_single_byte(cf_string_char(str, range.location + converted),
                                 encoding);
      if (b < 0) {
        if (!loss_byte) {
          break;
        }
        b = loss_byte;
      }
      if (buffer) {
        buffer[used] = b;
      }
      used++;
    }
  }
done:
  if (used_length) {
    *used_length = used;
  }
  return converted;
}

int CFStringGetCString(CFStringRef str, char* buffer, CFIndex size,
                       CFStringEncoding encoding) {
  if (size <= 0) {
    return 0;
  }
  CFIndex length = cf_string_length(str);
  CFIndex used;
  CFIndex converted = CFStringGetBytes(str, (CFRange){ 0, length }, encoding, 0,
                                       0, (UInt8*)buffer, size - 1, &used);
  buffer[used] = '\0';
  return converted == length;
}

int CFStringGetPascalString(CFStringRef str, unsigned char* buffer,
                            CFIndex size, CFStringEncoding encoding) {
  if (size <= 0) {
    return 0;
  }
  CFIndex room = size - 1 < 255 ? size - 1 : 255;
  CFIndex length = cf_string_length(str);
  CFIndex used;
  CFIndex converted = CFStringGetBytes(str, (CFRange){ 0, length }, encoding, 0,
                                       0, buffer + 1, room, &used);
  buffer[0] = used;
  return converted == length;
}

// ---------------------------------------------------------------------------
// Mutation

static UniChar fold(UniChar c) {
  return c < 0x80 ? (UniChar)tolower(c) : (UniChar)towlower(c);
}

enum {
  kCFCompareCaseInsensitive = 1,
  kCFCompareBackwards = 4,
  kCFCompareAnchored = 8,
};

static int matches_at(CFStringRef s, CFIndex at, CFStringRef find,
                      CFOptionFlags options) {
  CFIndex n = cf_string_length(find);
  for (CFIndex i = 0; i < n; i++) {
    UniChar a = cf_string_char(s, at + i);
    UniChar b = cf_string_char(find, i);
    if (options & kCFCompareCaseInsensitive) {
      a = fold(a);
      b = fold(b);
    }
    if (a != b) {
      return 0;
    }
  }
  return 1;
}

CFIndex CFStringFindAndReplace(CFMutableStringRef str, CFStringRef find,
                               CFStringRef replacement, CFRange range,
                               CFOptionFlags options) {
  if (is_constant(str) || !(str->flags & CF_STRING_MUTABLE)) {
    fprintf(stderr, "hle: CFStringFindAndReplace on an immutable string\n");
    return 0;
  }
  CFIndex find_length = cf_string_length(find);
  if (find_length == 0) {
    return 0;
  }
  CFIndex replacement_length;
  UniChar* replacement_chars = copy_chars(replacement, &replacement_length);
  CFIndex count = 0;
  CFIndex end = range.location + range.length;
  if (options & kCFCompareBackwards) {
    for (CFIndex at = end - find_length; at >= range.location; at--) {
      if (matches_at(str, at, find, options)) {
        string_replace(str, (CFRange){ at, find_length }, replacement_chars,
                       replacement_length);
        count++;
        at -= find_length - 1;
        if (options & kCFCompareAnchored) {
          break;
        }
      } else if (options & kCFCompareAnchored) {
        break;
      }
    }
  } else {
    CFIndex at = range.location;
    while (at + find_length <= end) {
      if (matches_at(str, at, find, options)) {
        string_replace(str, (CFRange){ at, find_length }, replacement_chars,
                       replacement_length);
        count++;
        at += replacement_length;
        end += replacement_length - find_length;
        if (options & kCFCompareAnchored) {
          break;
        }
      } else if (options & kCFCompareAnchored) {
        break;
      } else {
        at++;
      }
    }
  }
  free(replacement_chars);
  return count;
}

void CFStringLowercase(CFMutableStringRef str, CFLocaleRef locale) {
  if (is_constant(str) || !(str->flags & CF_STRING_MUTABLE)) {
    fprintf(stderr, "hle: CFStringLowercase on an immutable string\n");
    return;
  }
  for (CFIndex i = 0; i < str->length; i++) {
    str->chars[i] = fold(str->chars[i]);
  }
}

// ---------------------------------------------------------------------------
// Formatting: printf conversions, plus %@ for any CF object.

// A long double vararg takes 16 bytes on Darwin i386 and 12 on Linux.
typedef struct {
  unsigned char bytes[16];
} darwin_long_double;

static void format_into(cf_buf* out, const char* fmt, va_list ap) {
  const char* p = fmt;
  while (*p) {
    if (*p != '%') {
      const char* run = p;
      while (*p && *p != '%') {
        p++;
      }
      cf_buf_append(out, run, p - run);
      continue;
    }
    const char* start = p++;
    if (*p == '%') {
      cf_buf_append(out, "%", 1);
      p++;
      continue;
    }
    // Positional arguments are rare in this game's formats.
    const char* digits = p;
    while (*p >= '0' && *p <= '9') {
      p++;
    }
    if (*p == '$') {
      cf_warn_once("positional arguments in CFStringCreateWithFormat");
      start = p;
      p++;
    } else {
      p = digits;
    }
    char spec[32];
    size_t len = 0;
    spec[len++] = '%';
    while (*p && strchr("-+ #0'", *p) && len < 20) {
      spec[len++] = *p++;
    }
    int star_values[2];
    int stars = 0;
    for (int part = 0; part < 2; part++) {
      if (part == 1) {
        if (*p != '.') {
          break;
        }
        spec[len++] = *p++;
      }
      if (*p == '*') {
        star_values[stars++] = va_arg(ap, int);
        spec[len++] = *p++;
      } else {
        while (*p >= '0' && *p <= '9' && len < 28) {
          spec[len++] = *p++;
        }
      }
    }
    int longs = 0;
    int is_long_double = 0;
    int is_short = 0;
    for (;;) {
      if (*p == 'l') {
        longs++;
      } else if (*p == 'q') {
        longs = 2;
      } else if (*p == 'L') {
        is_long_double = 1;
      } else if (*p == 'h') {
        is_short = 1;
      } else if (*p != 'z' && *p != 't' && *p != 'j') {
        break;
      }
      p++;
    }
    char conv = *p ? *p++ : '\0';
    char buf[512];
    buf[0] = '\0';
    switch (conv) {
      case '@': {
        CFTypeRef obj = va_arg(ap, CFTypeRef);
        cf_describe(obj, out);
        continue;
      }
      case 'd':
      case 'i':
      case 'u':
      case 'o':
      case 'x':
      case 'X':
      case 'c': {
        if (longs >= 2) {
          spec[len++] = 'l';
          spec[len++] = 'l';
          spec[len++] = conv;
          spec[len] = '\0';
          long long v = va_arg(ap, long long);
          if (stars == 2) {
            snprintf(buf, sizeof(buf), spec, star_values[0], star_values[1], v);
          } else if (stars == 1) {
            snprintf(buf, sizeof(buf), spec, star_values[0], v);
          } else {
            snprintf(buf, sizeof(buf), spec, v);
          }
        } else {
          if (is_short && conv != 'c') {
            spec[len++] = 'h';
          }
          spec[len++] = conv;
          spec[len] = '\0';
          int v = va_arg(ap, int);
          if (stars == 2) {
            snprintf(buf, sizeof(buf), spec, star_values[0], star_values[1], v);
          } else if (stars == 1) {
            snprintf(buf, sizeof(buf), spec, star_values[0], v);
          } else {
            snprintf(buf, sizeof(buf), spec, v);
          }
        }
        break;
      }
      case 'e':
      case 'E':
      case 'f':
      case 'F':
      case 'g':
      case 'G':
      case 'a':
      case 'A': {
        long double v;
        if (is_long_double) {
          darwin_long_double raw = va_arg(ap, darwin_long_double);
          memset(&v, 0, sizeof(v));
          memcpy(&v, raw.bytes, 10);
          spec[len++] = 'L';
        } else {
          v = va_arg(ap, double);
          spec[len++] = 'L';
        }
        spec[len++] = conv;
        spec[len] = '\0';
        if (stars == 2) {
          snprintf(buf, sizeof(buf), spec, star_values[0], star_values[1], v);
        } else if (stars == 1) {
          snprintf(buf, sizeof(buf), spec, star_values[0], v);
        } else {
          snprintf(buf, sizeof(buf), spec, v);
        }
        break;
      }
      case 's': {
        // %s is in the system encoding, MacRoman.
        const char* s = va_arg(ap, const char*);
        if (!s) {
          s = "(null)";
        }
        CFStringRef tmp = CFStringCreateWithCString(NULL, s,
                                                    kCFStringEncodingMacRoman);
        char* utf8 = cf_string_utf8(tmp);
        spec[len++] = 's';
        spec[len] = '\0';
        cf_buf tmp_out = { 0 };
        if (stars == 2) {
          cf_buf_appendf(&tmp_out, spec, star_values[0], star_values[1], utf8);
        } else if (stars == 1) {
          cf_buf_appendf(&tmp_out, spec, star_values[0], utf8);
        } else {
          cf_buf_appendf(&tmp_out, spec, utf8);
        }
        if (tmp_out.data) {
          cf_buf_append(out, tmp_out.data, tmp_out.len);
        }
        cf_buf_free(&tmp_out);
        free(utf8);
        CFRelease(tmp);
        continue;
      }
      case 'S': {
        const UniChar* s = va_arg(ap, const UniChar*);
        CFIndex n = 0;
        while (s && s[n]) {
          n++;
        }
        CFStringRef tmp = CFStringCreateWithCharacters(NULL, s, n);
        cf_describe(tmp, out);
        CFRelease(tmp);
        continue;
      }
      case 'C': {
        UniChar c = va_arg(ap, int);
        CFStringRef tmp = CFStringCreateWithCharacters(NULL, &c, 1);
        cf_describe(tmp, out);
        CFRelease(tmp);
        continue;
      }
      case 'p':
        snprintf(buf, sizeof(buf), "%p", va_arg(ap, void*));
        break;
      default:
        cf_warn_once("a CFStringCreateWithFormat conversion");
        cf_buf_append(out, start, p - start);
        continue;
    }
    cf_buf_appends(out, buf);
  }
}

CFStringRef CFStringCreateWithFormatAndArguments(CFAllocatorRef allocator,
                                                 CFDictionaryRef options,
                                                 CFStringRef format,
                                                 va_list ap) {
  char* fmt = cf_string_utf8(format);
  cf_buf out = { 0 };
  format_into(&out, fmt, ap);
  CFStringRef result = cf_string_from_utf8(out.data ? out.data : "", out.len);
  cf_buf_free(&out);
  free(fmt);
  return result;
}

CFStringRef CFStringCreateWithFormat(CFAllocatorRef allocator,
                                     CFDictionaryRef options,
                                     CFStringRef format, ...) {
  va_list ap;
  va_start(ap, format);
  CFStringRef result =
      CFStringCreateWithFormatAndArguments(allocator, options, format, ap);
  va_end(ap);
  return result;
}
