// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// XML property lists: Info.plist, the HID string tables in the bundle, and
// the preferences this library writes. Binary plists never appear in the
// game's bundle and are not read.

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cf.h"

// ---------------------------------------------------------------------------
// Base64

static const char kBase64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int base64_value(char c) {
  const char* p = strchr(kBase64, c);
  return c && p ? (int)(p - kBase64) : -1;
}

static CFDataRef base64_decode(const char* text, size_t n) {
  UInt8* out = malloc(n * 3 / 4 + 3);
  size_t len = 0;
  unsigned acc = 0;
  int bits = 0;
  for (size_t i = 0; i < n; i++) {
    int v = base64_value(text[i]);
    if (v < 0) {
      continue;  // whitespace, line breaks and '=' padding
    }
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[len++] = (acc >> bits) & 0xff;
    }
  }
  CFDataRef data = cf_data_create(out, len);
  free(out);
  return data;
}

static void base64_encode(const UInt8* bytes, CFIndex n, cf_buf* out,
                          const char* indent) {
  for (CFIndex i = 0; i < n; i += 3) {
    if (i % 57 == 0) {
      if (i) {
        cf_buf_appends(out, "\n");
      }
      cf_buf_appends(out, indent);
    }
    unsigned v = bytes[i] << 16;
    if (i + 1 < n) {
      v |= bytes[i + 1] << 8;
    }
    if (i + 2 < n) {
      v |= bytes[i + 2];
    }
    char quad[4] = {
      kBase64[(v >> 18) & 63], kBase64[(v >> 12) & 63],
      i + 1 < n ? kBase64[(v >> 6) & 63] : '=',
      i + 2 < n ? kBase64[v & 63] : '=',
    };
    cf_buf_append(out, quad, 4);
  }
  cf_buf_appends(out, "\n");
}

// ---------------------------------------------------------------------------
// Parsing

typedef struct {
  const char* p;
  const char* end;
  const char* error;
} parser;

static void skip_space_and_comments(parser* ps) {
  for (;;) {
    while (ps->p < ps->end &&
           (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' ||
            *ps->p == '\r')) {
      ps->p++;
    }
    if (ps->end - ps->p >= 4 && !memcmp(ps->p, "<!--", 4)) {
      const char* close = memmem(ps->p + 4, ps->end - ps->p - 4, "-->", 3);
      ps->p = close ? close + 3 : ps->end;
    } else if (ps->end - ps->p >= 2 &&
               (!memcmp(ps->p, "<?", 2) ||
                (!memcmp(ps->p, "<!", 2) &&
                 memcmp(ps->p, "<![CDATA[", ps->end - ps->p < 9 ? 0 : 9)))) {
      const char* close = memchr(ps->p, '>', ps->end - ps->p);
      ps->p = close ? close + 1 : ps->end;
    } else {
      return;
    }
  }
}

// Reads "<name ...>" or "<name/>". Returns 1 for an open tag, 2 for an empty
// one, 0 when the next thing is not a tag. |name| gets the element name.
static int read_open_tag(parser* ps, char* name, size_t size) {
  skip_space_and_comments(ps);
  if (ps->p >= ps->end || *ps->p != '<' ||
      (ps->p + 1 < ps->end && ps->p[1] == '/')) {
    return 0;
  }
  const char* close = memchr(ps->p, '>', ps->end - ps->p);
  if (!close) {
    ps->error = "unterminated tag";
    return 0;
  }
  const char* q = ps->p + 1;
  size_t n = 0;
  while (q < close && *q != ' ' && *q != '\t' && *q != '\n' && *q != '/' &&
         n + 1 < size) {
    name[n++] = *q++;
  }
  name[n] = '\0';
  int empty = close[-1] == '/';
  ps->p = close + 1;
  return empty ? 2 : 1;
}

static int expect_close_tag(parser* ps, const char* name) {
  skip_space_and_comments(ps);
  size_t n = strlen(name);
  if (ps->end - ps->p < (ptrdiff_t)(n + 3) || memcmp(ps->p, "</", 2) ||
      memcmp(ps->p + 2, name, n) || ps->p[2 + n] != '>') {
    ps->error = "mismatched closing tag";
    return 0;
  }
  ps->p += n + 3;
  return 1;
}

static void append_utf8(cf_buf* out, unsigned long c) {
  char tmp[4];
  int n;
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
  cf_buf_append(out, tmp, n);
}

// The character data up to the next "</", with entities and CDATA decoded.
static void read_text(parser* ps, cf_buf* out) {
  cf_buf_append(out, "", 0);
  while (ps->p < ps->end) {
    if (ps->end - ps->p >= 9 && !memcmp(ps->p, "<![CDATA[", 9)) {
      const char* close = memmem(ps->p + 9, ps->end - ps->p - 9, "]]>", 3);
      if (!close) {
        ps->error = "unterminated CDATA";
        return;
      }
      cf_buf_append(out, ps->p + 9, close - (ps->p + 9));
      ps->p = close + 3;
    } else if (*ps->p == '<') {
      return;
    } else if (*ps->p == '&') {
      const char* semi = memchr(ps->p, ';', ps->end - ps->p);
      if (!semi) {
        ps->error = "unterminated entity";
        return;
      }
      const char* e = ps->p + 1;
      size_t n = semi - e;
      if (n == 2 && !memcmp(e, "lt", 2)) {
        cf_buf_append(out, "<", 1);
      } else if (n == 2 && !memcmp(e, "gt", 2)) {
        cf_buf_append(out, ">", 1);
      } else if (n == 3 && !memcmp(e, "amp", 3)) {
        cf_buf_append(out, "&", 1);
      } else if (n == 4 && !memcmp(e, "quot", 4)) {
        cf_buf_append(out, "\"", 1);
      } else if (n == 4 && !memcmp(e, "apos", 4)) {
        cf_buf_append(out, "'", 1);
      } else if (n >= 2 && e[0] == '#') {
        unsigned long c = e[1] == 'x' ? strtoul(e + 2, NULL, 16)
                                      : strtoul(e + 1, NULL, 10);
        append_utf8(out, c);
      } else {
        cf_buf_append(out, ps->p, semi + 1 - ps->p);
      }
      ps->p = semi + 1;
    } else {
      const char* run = ps->p;
      while (ps->p < ps->end && *ps->p != '<' && *ps->p != '&') {
        ps->p++;
      }
      cf_buf_append(out, run, ps->p - run);
    }
  }
}

static CFTypeRef parse_object(parser* ps, int depth);

static CFTypeRef parse_text_element(parser* ps, const char* name, int empty,
                                    cf_buf* text) {
  if (!empty) {
    read_text(ps, text);
    if (ps->error || !expect_close_tag(ps, name)) {
      return NULL;
    }
  } else {
    cf_buf_append(text, "", 0);
  }
  return kCFBooleanTrue;  // any non-NULL: the text is in |text|
}

static CFTypeRef parse_object(parser* ps, int depth) {
  char name[32];
  int kind = read_open_tag(ps, name, sizeof(name));
  if (kind == 0) {
    if (!ps->error) {
      ps->error = "expected an element";
    }
    return NULL;
  }
  if (depth > 64) {
    ps->error = "nested too deeply";
    return NULL;
  }
  int empty = kind == 2;

  if (!strcmp(name, "dict")) {
    CFMutableDictionaryRef dict = cf_dict_create();
    while (!empty) {
      char key_tag[32];
      skip_space_and_comments(ps);
      if (ps->end - ps->p >= 2 && !memcmp(ps->p, "</", 2)) {
        break;
      }
      int key_kind = read_open_tag(ps, key_tag, sizeof(key_tag));
      if (key_kind == 0 || strcmp(key_tag, "key")) {
        ps->error = ps->error ? ps->error : "expected <key> in <dict>";
        CFRelease(dict);
        return NULL;
      }
      cf_buf key_text = { 0 };
      if (!parse_text_element(ps, "key", key_kind == 2, &key_text)) {
        cf_buf_free(&key_text);
        CFRelease(dict);
        return NULL;
      }
      CFTypeRef value = parse_object(ps, depth + 1);
      if (!value) {
        cf_buf_free(&key_text);
        CFRelease(dict);
        return NULL;
      }
      CFStringRef key = cf_string_from_utf8(key_text.data, key_text.len);
      cf_dict_set(dict, key, value);
      CFRelease(key);
      CFRelease(value);
      cf_buf_free(&key_text);
    }
    if (!empty && !expect_close_tag(ps, "dict")) {
      CFRelease(dict);
      return NULL;
    }
    return dict;
  }

  if (!strcmp(name, "array")) {
    CFMutableArrayRef array = cf_array_create();
    while (!empty) {
      skip_space_and_comments(ps);
      if (ps->end - ps->p >= 2 && !memcmp(ps->p, "</", 2)) {
        break;
      }
      CFTypeRef value = parse_object(ps, depth + 1);
      if (!value) {
        CFRelease(array);
        return NULL;
      }
      cf_array_append(array, value);
      CFRelease(value);
    }
    if (!empty && !expect_close_tag(ps, "array")) {
      CFRelease(array);
      return NULL;
    }
    return array;
  }

  if (!strcmp(name, "true") || !strcmp(name, "false")) {
    if (!empty && !expect_close_tag(ps, name)) {
      return NULL;
    }
    return name[0] == 't' ? kCFBooleanTrue : kCFBooleanFalse;
  }

  cf_buf text = { 0 };
  if (!parse_text_element(ps, name, empty, &text)) {
    cf_buf_free(&text);
    return NULL;
  }
  CFTypeRef result = NULL;
  if (!strcmp(name, "string") || !strcmp(name, "date")) {
    if (name[0] == 'd') {
      cf_warn_once("<date> in a property list (kept as a string)");
    }
    result = cf_string_from_utf8(text.data, text.len);
  } else if (!strcmp(name, "integer")) {
    result = cf_number_int(strtoll(text.data, NULL, 0));
  } else if (!strcmp(name, "real")) {
    result = cf_number_double(strtod(text.data, NULL));
  } else if (!strcmp(name, "data")) {
    result = base64_decode(text.data, text.len);
  } else {
    ps->error = "unknown property list element";
  }
  cf_buf_free(&text);
  return result;
}

CFPropertyListRef cf_plist_parse(const char* xml, size_t len,
                                 const char** error) {
  parser ps = { xml, xml + len, NULL };
  char name[32];
  const char* before = ps.p;
  skip_space_and_comments(&ps);
  const char* root = ps.p;
  int kind = read_open_tag(&ps, name, sizeof(name));
  CFTypeRef result = NULL;
  if (kind != 0 && !strcmp(name, "plist")) {
    if (kind == 1) {
      result = parse_object(&ps, 0);
      if (result && !expect_close_tag(&ps, "plist")) {
        CFRelease(result);
        result = NULL;
      }
    }
  } else if (!ps.error) {
    // A bare top-level object without <plist>.
    ps.p = root;
    result = parse_object(&ps, 0);
  }
  (void)before;
  if (!result && error) {
    *error = ps.error ? ps.error : "not a property list";
  }
  return result;
}

CFPropertyListRef cf_plist_read_file(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  cf_buf contents = { 0 };
  char chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
    cf_buf_append(&contents, chunk, n);
  }
  fclose(f);
  const char* error = NULL;
  CFPropertyListRef plist =
      contents.data ? cf_plist_parse(contents.data, contents.len, &error)
                    : NULL;
  if (!plist) {
    fprintf(stderr, "hle: %s: %s\n", path, error ? error : "empty");
  }
  cf_buf_free(&contents);
  return plist;
}

enum {
  kCFPropertyListImmutable = 0,
  kCFPropertyListMutableContainers = 1,
  kCFPropertyListMutableContainersAndLeaves = 2,
};

// Parsed containers are always mutable here.
CFPropertyListRef CFPropertyListCreateFromXMLData(CFAllocatorRef allocator,
                                                  CFDataRef xml,
                                                  CFOptionFlags mutability,
                                                  CFStringRef* error_string) {
  const char* error = NULL;
  CFPropertyListRef plist = cf_plist_parse((const char*)cf_data_bytes(xml),
                                           cf_data_length(xml), &error);
  // CF clears the error string on success, and the game counts on it: it
  // passes an uninitialized one and releases whatever comes back.
  if (error_string) {
    *error_string = plist ? NULL : cf_string_from_utf8(error, strlen(error));
  }
  return plist;
}

// ---------------------------------------------------------------------------
// Writing

static void write_escaped(cf_buf* out, CFStringRef s) {
  char* utf8 = cf_string_utf8(s);
  for (const char* p = utf8; *p; p++) {
    switch (*p) {
      case '&':
        cf_buf_appends(out, "&amp;");
        break;
      case '<':
        cf_buf_appends(out, "&lt;");
        break;
      case '>':
        cf_buf_appends(out, "&gt;");
        break;
      default:
        cf_buf_append(out, p, 1);
    }
  }
  free(utf8);
}

static void write_object(cf_buf* out, CFTypeRef obj, int depth) {
  char indent[65];
  int n = depth < 64 ? depth : 64;
  memset(indent, '\t', n);
  indent[n] = '\0';
  cf_buf_appends(out, indent);
  switch (CFGetTypeID(obj)) {
    case CF_TYPE_DICTIONARY: {
      CFIndex count = cf_dict_count(obj);
      if (!count) {
        cf_buf_appends(out, "<dict/>\n");
        break;
      }
      cf_buf_appends(out, "<dict>\n");
      for (CFIndex i = 0; i < count; i++) {
        CFTypeRef key;
        CFTypeRef value;
        cf_dict_pair(obj, i, &key, &value);
        if (!cf_is(key, CF_TYPE_STRING)) {
          continue;
        }
        cf_buf_appends(out, indent);
        cf_buf_appends(out, "\t<key>");
        write_escaped(out, key);
        cf_buf_appends(out, "</key>\n");
        write_object(out, value, depth + 1);
      }
      cf_buf_appends(out, indent);
      cf_buf_appends(out, "</dict>\n");
      break;
    }
    case CF_TYPE_ARRAY: {
      CFIndex count = cf_array_count(obj);
      if (!count) {
        cf_buf_appends(out, "<array/>\n");
        break;
      }
      cf_buf_appends(out, "<array>\n");
      for (CFIndex i = 0; i < count; i++) {
        write_object(out, cf_array_get(obj, i), depth + 1);
      }
      cf_buf_appends(out, indent);
      cf_buf_appends(out, "</array>\n");
      break;
    }
    case CF_TYPE_STRING:
      cf_buf_appends(out, "<string>");
      write_escaped(out, obj);
      cf_buf_appends(out, "</string>\n");
      break;
    case CF_TYPE_NUMBER:
      if (cf_number_is_float(obj)) {
        cf_buf_appendf(out, "<real>%.17g</real>\n", cf_number_as_double(obj));
      } else {
        cf_buf_appendf(out, "<integer>%lld</integer>\n",
                       (long long)cf_number_as_int(obj));
      }
      break;
    case CF_TYPE_BOOLEAN:
      cf_buf_appends(out, obj == kCFBooleanTrue ? "<true/>\n" : "<false/>\n");
      break;
    case CF_TYPE_DATA:
      cf_buf_appends(out, "<data>\n");
      base64_encode(cf_data_bytes(obj), cf_data_length(obj), out, indent);
      cf_buf_appends(out, indent);
      cf_buf_appends(out, "</data>\n");
      break;
    default:
      cf_warn_once("writing a CF type property lists cannot hold");
      cf_buf_appends(out, "<string></string>\n");
  }
}

void cf_plist_write(CFPropertyListRef plist, cf_buf* out) {
  cf_buf_appends(out,
                 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                 "<!DOCTYPE plist PUBLIC \"-//Apple Computer//DTD PLIST 1.0//EN\" "
                 "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                 "<plist version=\"1.0\">\n");
  write_object(out, plist, 0);
  cf_buf_appends(out, "</plist>\n");
}

// Written beside the target and renamed over it, so a crash mid-write never
// leaves half a file.
int cf_plist_write_file(CFPropertyListRef plist, const char* path) {
  cf_buf out = { 0 };
  cf_plist_write(plist, &out);
  size_t tmp_len = strlen(path) + 16;
  char* tmp = malloc(tmp_len);
  snprintf(tmp, tmp_len, "%s.tmp%d", path, (int)getpid());
  FILE* f = fopen(tmp, "wb");
  int ok = f && fwrite(out.data, 1, out.len, f) == out.len;
  if (f) {
    ok = (fclose(f) == 0) && ok;
  }
  if (ok) {
    ok = rename(tmp, path) == 0;
  }
  if (!ok) {
    fprintf(stderr, "hle: cannot write %s: %s\n", path, strerror(errno));
    unlink(tmp);
  }
  free(tmp);
  cf_buf_free(&out);
  return ok;
}

CFDataRef CFPropertyListCreateXMLData(CFAllocatorRef allocator,
                                      CFPropertyListRef plist) {
  cf_buf out = { 0 };
  cf_plist_write(plist, &out);
  CFDataRef data = cf_data_create((const UInt8*)out.data, out.len);
  cf_buf_free(&out);
  return data;
}
