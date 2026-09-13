// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// The Text Encoding Converter, for the one conversion the game makes:
// Unicode to Mac Roman, through CFString's tables.

#include <stdlib.h>

#include "carbon.h"

enum {
  kTextEncodingMacRoman = 0,
  kTextEncodingUnicodeDefault = 0x100,
  kTECUnmappableElementErr = -8754,
  kTECOutputBufferFullStatus = -8785,
  kTextUnsupportedEncodingErr = -8738,
  kUnicodeUseFallbacksMask = 1 << 0,
};

enum {
  kMagicUnicodeToText = 'u2tx',
};

#pragma pack(push, 2)
typedef struct {
  TextEncoding unicodeEncoding;
  TextEncoding otherEncoding;
  int32_t mappingVersion;
} UnicodeMapping;
#pragma pack(pop)

typedef struct {
  uint32_t magic;
  TextEncoding other;
} unicode_to_text;

TextEncoding CreateTextEncoding(uint32_t base, uint32_t variant,
                                uint32_t format) {
  return (format & 0x3F) << 26 | (variant & 0x3FF) << 16 | (base & 0xFFFF);
}

uint32_t GetTextEncodingBase(TextEncoding encoding) {
  return encoding & 0xFFFF;
}

// Every script this Mac has is Roman.
int UpgradeScriptInfoToTextEncoding(SInt16 script, SInt16 language,
                                    SInt16 region, ConstStringPtr font,
                                    TextEncoding* out) {
  if (out) {
    *out = kTextEncodingMacRoman;
  }
  return noErr;
}

int CreateUnicodeToTextInfo(const UnicodeMapping* mapping,
                            unicode_to_text** out) {
  if (!mapping || !out) {
    return paramErr;
  }
  if (GetTextEncodingBase(mapping->otherEncoding) != kTextEncodingMacRoman) {
    *out = NULL;
    return kTextUnsupportedEncodingErr;
  }
  unicode_to_text* info = calloc(1, sizeof(*info));
  info->magic = kMagicUnicodeToText;
  info->other = mapping->otherEncoding;
  *out = info;
  return noErr;
}

int DisposeUnicodeToTextInfo(unicode_to_text** info) {
  if (info && *info && (*info)->magic == kMagicUnicodeToText) {
    (*info)->magic = 0;
    free(*info);
    *info = NULL;
  }
  return noErr;
}

int ConvertFromUnicodeToText(unicode_to_text* info, ByteCount unicode_bytes,
                             const UniChar* unicode, uint32_t flags,
                             ItemCount offset_count, void* offsets,
                             ItemCount* out_offset_count, void* out_offsets,
                             ByteCount out_size, ByteCount* read,
                             ByteCount* written, void* out) {
  if (!info || info->magic != kMagicUnicodeToText || (!unicode && unicode_bytes)) {
    return paramErr;
  }
  if (out_offset_count) {
    *out_offset_count = 0;
  }
  CFIndex chars = unicode_bytes / sizeof(UniChar);
  CFStringRef s = CFStringCreateWithCharacters(NULL, unicode, chars);
  UInt8 loss = (flags & kUnicodeUseFallbacksMask) ? '?' : 0;
  CFIndex used = 0;
  CFRange range = { 0, chars };
  CFIndex converted = CFStringGetBytes(s, range, kCFStringEncodingMacRoman,
                                       loss, 0, (UInt8*)out, out_size, &used);
  CFRelease(s);
  if (read) {
    *read = converted * sizeof(UniChar);
  }
  if (written) {
    *written = used;
  }
  if (converted < chars) {
    return used >= (CFIndex)out_size ? kTECOutputBufferFullStatus
                                     : kTECUnmappableElementErr;
  }
  return noErr;
}
