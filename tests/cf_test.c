// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Checks the CoreFoundation in hle/ against libmac.so. This exercises the
// logic from Linux-compiled code, not the Darwin calling convention.
//
//   make tests/cf_test
//   tests/cf_test [/path/to/Halo.app]
//
// With a bundle path it also reads the game's Info.plist, resources and
// localized strings.

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../hle/cf.h"

CFStringRef CFStringCreateWithCString(CFAllocatorRef, const char*,
                                      CFStringEncoding);
int CFStringGetCString(CFStringRef, char*, CFIndex, CFStringEncoding);
CFIndex CFStringGetLength(CFStringRef);
unsigned int CFStringGetCharacterAtIndex(CFStringRef, CFIndex);
CFStringRef CFStringCreateWithFormat(CFAllocatorRef, CFDictionaryRef,
                                     CFStringRef, ...);
CFMutableStringRef CFStringCreateMutableCopy(CFAllocatorRef, CFIndex,
                                             CFStringRef);
CFMutableStringRef CFStringCreateMutableWithExternalCharactersNoCopy(
    CFAllocatorRef, UniChar*, CFIndex, CFIndex, CFAllocatorRef);
CFIndex CFStringFindAndReplace(CFMutableStringRef, CFStringRef, CFStringRef,
                               CFRange, CFOptionFlags);
void CFStringLowercase(CFMutableStringRef, CFLocaleRef);
int CFStringGetPascalString(CFStringRef, unsigned char*, CFIndex,
                            CFStringEncoding);
CFIndex CFStringGetBytes(CFStringRef, CFRange, CFStringEncoding, unsigned int,
                         unsigned int, UInt8*, CFIndex, CFIndex*);
CFNumberRef CFNumberCreate(CFAllocatorRef, CFIndex, const void*);
int CFNumberGetValue(CFNumberRef, CFIndex, void*);
CFUUIDRef CFUUIDGetConstantUUIDWithBytes(
    CFAllocatorRef, unsigned int, unsigned int, unsigned int, unsigned int,
    unsigned int, unsigned int, unsigned int, unsigned int, unsigned int,
    unsigned int, unsigned int, unsigned int, unsigned int, unsigned int,
    unsigned int, unsigned int);
CFUUIDBytes* CFUUIDGetUUIDBytes(CFUUIDBytes*, CFUUIDRef);
CFTypeRef CFBundleGetValueForInfoDictionaryKey(CFBundleRef, CFStringRef);
CFURLRef CFBundleCopyResourceURL(CFBundleRef, CFStringRef, CFStringRef,
                                 CFStringRef);
CFStringRef CFBundleCopyLocalizedString(CFBundleRef, CFStringRef, CFStringRef,
                                        CFStringRef);
CFStringRef CFURLCopyLastPathComponent(CFURLRef);
CFPropertyListRef CFPreferencesCopyAppValue(CFStringRef, CFStringRef);
void CFPreferencesSetAppValue(CFStringRef, CFPropertyListRef, CFStringRef);
int CFPreferencesAppSynchronize(CFStringRef);

typedef struct {
  CFRuntimeBase base;
  const char* bytes;
  long length;
} constant_string;

#define CONSTANT(name, text)                                             \
  static constant_string name = {                                        \
    { __CFConstantStringClassReference, 0x07c8 }, text, sizeof(text) - 1 \
  }

CONSTANT(k_hello, "Hello");
CONSTANT(k_format, "%@ %d %s 0x%4.4lX %5.2f");
CONSTANT(k_l, "l");
CONSTANT(k_upper_l, "L");
CONSTANT(k_b, "b");
CONSTANT(k_bb, "BB");
CONSTANT(k_identifier, "CFBundleIdentifier");
CONSTANT(k_short_version, "CFBundleShortVersionString");
CONSTANT(k_hid_usage, "hid_usage_strings");
CONSTANT(k_plist_type, "plist");
CONSTANT(k_test_key, "HleTestKey");
CONSTANT(k_need_opengl, "MsgNeedOpenGL");

#define S(name) ((CFStringRef)&(name))

static int failures;

static void check(int ok, const char* what) {
  printf("%s - %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) {
    failures++;
  }
}

static int string_is(CFStringRef s, const char* utf8) {
  char buf[512];
  return s && CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8) &&
         !strcmp(buf, utf8);
}

static void test_strings(void) {
  check(CFStringGetLength(S(k_hello)) == 5, "constant string length");
  check(cf_is(S(k_hello), CF_TYPE_STRING), "constant string type");

  CFStringRef cafe = CFStringCreateWithCString(NULL, "caf\x8e",
                                               kCFStringEncodingMacRoman);
  check(CFStringGetCharacterAtIndex(cafe, 3) == 0x00E9, "MacRoman decode");
  check(string_is(cafe, "caf\xc3\xa9"), "UTF-8 encode");
  char roman[8];
  check(CFStringGetCString(cafe, roman, sizeof(roman),
                           kCFStringEncodingMacRoman) &&
            !strcmp(roman, "caf\x8e"),
        "MacRoman round trip");
  char ascii[8];
  check(!CFStringGetCString(cafe, ascii, sizeof(ascii),
                            kCFStringEncodingASCII),
        "ASCII refuses e-acute");
  CFRelease(cafe);

  CFStringRef formatted = CFStringCreateWithFormat(
      NULL, NULL, S(k_format), S(k_hello), 42, "x", 0xabcdL, 3.14159);
  check(string_is(formatted, "Hello 42 x 0xABCD  3.14"), "format with %@");
  CFRelease(formatted);

  CFMutableStringRef mutable_copy = CFStringCreateMutableCopy(NULL, 0,
                                                              S(k_hello));
  check(CFStringFindAndReplace(mutable_copy, S(k_l), S(k_upper_l),
                               (CFRange){ 0, 5 }, 0) == 2 &&
            string_is(mutable_copy, "HeLLo"),
        "find and replace");
  CFStringLowercase(mutable_copy, NULL);
  check(string_is(mutable_copy, "hello"), "lowercase");
  CFRelease(mutable_copy);

  unsigned char pascal[16];
  check(CFStringGetPascalString(S(k_hello), pascal, sizeof(pascal),
                                kCFStringEncodingMacRoman) &&
            pascal[0] == 5 && !memcmp(pascal + 1, "Hello", 5),
        "Pascal string");

  UInt8 utf16[16];
  CFIndex used;
  check(CFStringGetBytes(S(k_hello), (CFRange){ 0, 5 },
                         kCFStringEncodingUTF16BE, 0, 0, utf16, sizeof(utf16),
                         &used) == 5 &&
            used == 10 && utf16[0] == 0 && utf16[1] == 'H',
        "UTF-16BE bytes");

  UniChar external[8] = { 'a', 'b', 'c' };
  CFMutableStringRef ext = CFStringCreateMutableWithExternalCharactersNoCopy(
      NULL, external, 3, 8, kCFAllocatorNull);
  CFStringFindAndReplace(ext, S(k_b), S(k_bb), (CFRange){ 0, 3 }, 0);
  check(CFStringGetLength(ext) == 4 && external[1] == 'B' &&
            external[2] == 'B' && external[3] == 'c',
        "external characters stay in the caller's buffer");
  CFRelease(ext);
}

static void test_plist(void) {
  static const char xml[] =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<!DOCTYPE plist PUBLIC \"-//Apple Computer//DTD PLIST 1.0//EN\" "
      "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
      "<plist version=\"1.0\"><dict>\n"
      "  <!-- a comment -->\n"
      "  <key>Name</key><string>Blood &amp; Gulch</string>\n"
      "  <key>Count</key><integer>31</integer>\n"
      "  <key>Gamma</key><real>1.5</real>\n"
      "  <key>Fullscreen</key><true/>\n"
      "  <key>Blob</key><data>SGFsbw==</data>\n"
      "  <key>Maps</key><array><string>a10</string><string/></array>\n"
      "  <key>Empty</key><dict/>\n"
      "</dict></plist>\n";
  const char* error = NULL;
  CFPropertyListRef plist = cf_plist_parse(xml, sizeof(xml) - 1, &error);
  check(plist && cf_is(plist, CF_TYPE_DICTIONARY), "plist parses");
  if (!plist) {
    printf("  error: %s\n", error);
    return;
  }
  check(string_is(cf_dict_get_ascii(plist, "Name"), "Blood & Gulch"),
        "plist entity");
  CFTypeRef count = cf_dict_get_ascii(plist, "Count");
  check(count && cf_number_as_int(count) == 31, "plist integer");
  CFTypeRef gamma = cf_dict_get_ascii(plist, "Gamma");
  check(gamma && cf_number_as_double(gamma) == 1.5, "plist real");
  check(cf_dict_get_ascii(plist, "Fullscreen") == kCFBooleanTrue,
        "plist true");
  CFTypeRef blob = cf_dict_get_ascii(plist, "Blob");
  check(blob && cf_data_length(blob) == 4 &&
            !memcmp(cf_data_bytes(blob), "Halo", 4),
        "plist data");
  CFTypeRef maps = cf_dict_get_ascii(plist, "Maps");
  check(maps && cf_array_count(maps) == 2 &&
            string_is(cf_array_get(maps, 1), ""),
        "plist array with an empty string");

  cf_buf written = { 0 };
  cf_plist_write(plist, &written);
  CFPropertyListRef again = cf_plist_parse(written.data, written.len, &error);
  check(again && cf_equal(plist, again), "plist write and re-read");
  cf_buf_free(&written);
  if (again) {
    CFRelease(again);
  }
  CFRelease(plist);
}

static void test_scalars(void) {
  int32_t in = -7;
  CFNumberRef n = CFNumberCreate(NULL, 3, &in);
  double d = 0;
  check(CFNumberGetValue(n, 13, &d) && d == -7.0, "number as double");
  int8_t small = 0;
  check(CFNumberGetValue(n, 1, &small) && small == -7, "number as SInt8");
  CFRelease(n);

  CFUUIDRef a = CFUUIDGetConstantUUIDWithBytes(NULL, 1, 2, 3, 4, 5, 6, 7, 8,
                                               9, 10, 11, 12, 13, 14, 15, 16);
  CFUUIDRef b = CFUUIDGetConstantUUIDWithBytes(NULL, 1, 2, 3, 4, 5, 6, 7, 8,
                                               9, 10, 11, 12, 13, 14, 15, 16);
  CFUUIDBytes bytes;
  check(a == b, "UUIDs intern");
  check(CFUUIDGetUUIDBytes(&bytes, a) == &bytes && bytes.bytes[0] == 1 &&
            bytes.bytes[15] == 16,
        "UUID bytes through the hidden pointer");
}

static void test_bundle(void) {
  CFBundleRef bundle = CFBundleGetMainBundle();
  check(string_is(CFBundleGetValueForInfoDictionaryKey(bundle,
                                                       S(k_identifier)),
                  "com.macsoft.halo"),
        "Info.plist identifier");
  CFTypeRef version = CFBundleGetValueForInfoDictionaryKey(bundle,
                                                           S(k_short_version));
  char buf[256] = "";
  if (version) {
    CFStringGetCString(version, buf, sizeof(buf), kCFStringEncodingUTF8);
  }
  check(version != NULL, "Info.plist short version");
  printf("  short version: %s\n", buf);

  CFURLRef url = CFBundleCopyResourceURL(bundle, S(k_hid_usage),
                                         S(k_plist_type), NULL);
  CFStringRef last = url ? CFURLCopyLastPathComponent(url) : NULL;
  check(string_is(last, "HID_usage_strings.plist"),
        "resource found despite case");
  if (url) {
    CFTypeRef plist = cf_plist_read_file(
        (const char*)((const struct { CFObject o; char* path; }*)url)->path);
    check(plist && cf_is(plist, CF_TYPE_DICTIONARY),
          "HID_usage_strings.plist parses");
    if (plist) {
      CFRelease(plist);
    }
    CFRelease(url);
  }
  if (last) {
    CFRelease(last);
  }

  CFStringRef message = CFBundleCopyLocalizedString(bundle, S(k_need_opengl),
                                                    NULL, NULL);
  buf[0] = '\0';
  CFStringGetCString(message, buf, sizeof(buf), kCFStringEncodingUTF8);
  check(!strncmp(buf, "<<<kGameName>>> requires OpenGL version", 39),
        "Localizable.strings (UTF-16) lookup");
  printf("  MsgNeedOpenGL -> %s\n", buf);
  CFRelease(message);
}

static void test_prefs(void) {
  char dir[] = "/tmp/hle-cf-test-XXXXXX";
  if (!mkdtemp(dir)) {
    check(0, "temporary home");
    return;
  }
  setenv("HALO_MAC_HOME", dir, 1);
  CFPreferencesSetAppValue(S(k_test_key), S(k_hello),
                           kCFPreferencesCurrentApplication);
  check(CFPreferencesAppSynchronize(kCFPreferencesCurrentApplication),
        "preferences synchronize");
  CFPropertyListRef value = CFPreferencesCopyAppValue(
      S(k_test_key), kCFPreferencesCurrentApplication);
  check(string_is(value, "Hello"), "preferences read back");
  if (value) {
    CFRelease(value);
  }
  char cmd[512];
  snprintf(cmd, sizeof(cmd), "grep -q HleTestKey %s/Library/Preferences/*.plist",
           dir);
  check(system(cmd) == 0, "preferences file on disk");
  snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
  system(cmd);
}

int main(int argc, char** argv) {
  if (argc > 1) {
    // Before anything asks for the main bundle, which is made once.
    snprintf(__darwin_executable_path, 4096, "%s/Contents/MacOS/Halo",
             argv[1]);
  }
  test_prefs();  // before the rest: HALO_MAC_HOME is read once
  test_strings();
  test_plist();
  test_scalars();
  if (argc > 1) {
    test_bundle();
  }
  printf("%s\n", failures ? "FAILED" : "all passed");
  return failures ? 1 : 0;
}
