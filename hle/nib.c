// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

#define _GNU_SOURCE

#include "nib.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cf.h"

CFURLRef CFBundleCopyResourceURL(CFBundleRef bundle, CFStringRef name,
                                 CFStringRef type, CFStringRef subdir);
int CFURLGetFileSystemRepresentation(CFURLRef url, unsigned int resolve,
                                     UInt8* buffer, CFIndex size);

struct hle_nib {
  xml_node* root;
  // Every element with an id attribute, for references.
  xml_node** by_id;
  int id_count;
};

// ---------------------------------------------------------------------------
// A small XML reader. IB's files are well-formed apart from the odd stray
// byte, which is kept as text rather than refused.

typedef struct {
  const char* p;
  const char* end;
} reader;

static void append(char** s, size_t* len, const char* add, size_t n) {
  *s = realloc(*s, *len + n + 1);
  memcpy(*s + *len, add, n);
  *len += n;
  (*s)[*len] = '\0';
}

static void append_code_point(char** s, size_t* len, unsigned long c) {
  char utf8[4];
  int n;
  if (c < 0x80) {
    utf8[0] = c;
    n = 1;
  } else if (c < 0x800) {
    utf8[0] = 0xC0 | (c >> 6);
    utf8[1] = 0x80 | (c & 0x3F);
    n = 2;
  } else {
    utf8[0] = 0xE0 | (c >> 12);
    utf8[1] = 0x80 | ((c >> 6) & 0x3F);
    utf8[2] = 0x80 | (c & 0x3F);
    n = 3;
  }
  append(s, len, utf8, n);
}

// Decodes entities in [p, end) into a malloc'd string.
static char* decode(const char* p, const char* end) {
  char* out = NULL;
  size_t len = 0;
  append(&out, &len, "", 0);
  while (p < end) {
    const char* amp = memchr(p, '&', end - p);
    const char* stop = amp ? amp : end;
    append(&out, &len, p, stop - p);
    p = stop;
    if (!amp) {
      break;
    }
    const char* semi = memchr(amp, ';', end - amp);
    if (!semi || semi - amp > 10) {
      append(&out, &len, "&", 1);
      p = amp + 1;
      continue;
    }
    size_t n = semi - amp - 1;
    const char* e = amp + 1;
    if (n == 3 && !memcmp(e, "amp", 3)) {
      append(&out, &len, "&", 1);
    } else if (n == 2 && !memcmp(e, "lt", 2)) {
      append(&out, &len, "<", 1);
    } else if (n == 2 && !memcmp(e, "gt", 2)) {
      append(&out, &len, ">", 1);
    } else if (n == 4 && !memcmp(e, "quot", 4)) {
      append(&out, &len, "\"", 1);
    } else if (n == 4 && !memcmp(e, "apos", 4)) {
      append(&out, &len, "'", 1);
    } else if (n > 1 && e[0] == '#') {
      append_code_point(&out, &len, e[1] == 'x' ? strtoul(e + 2, NULL, 16)
                                                : strtoul(e + 1, NULL, 10));
    } else {
      append(&out, &len, amp, semi + 1 - amp);
    }
    p = semi + 1;
  }
  return out;
}

static void skip_misc(reader* r) {
  for (;;) {
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' ||
                             *r->p == '\n' || *r->p == '\r')) {
      r->p++;
    }
    if (r->end - r->p >= 4 && !memcmp(r->p, "<!--", 4)) {
      const char* close = memmem(r->p, r->end - r->p, "-->", 3);
      r->p = close ? close + 3 : r->end;
    } else if (r->end - r->p >= 2 &&
               (!memcmp(r->p, "<?", 2) ||
                (!memcmp(r->p, "<!", 2) &&
                 (r->end - r->p < 9 || memcmp(r->p, "<![CDATA[", 9))))) {
      const char* close = memchr(r->p, '>', r->end - r->p);
      r->p = close ? close + 1 : r->end;
    } else {
      return;
    }
  }
}

static xml_node* parse_element(reader* r) {
  skip_misc(r);
  if (r->p >= r->end || *r->p != '<' || r->p[1] == '/') {
    return NULL;
  }
  const char* close = memchr(r->p, '>', r->end - r->p);
  if (!close) {
    return NULL;
  }
  xml_node* node = calloc(1, sizeof(*node));
  const char* q = r->p + 1;
  const char* name_end = q;
  while (name_end < close && *name_end != ' ' && *name_end != '\t' &&
         *name_end != '\n' && *name_end != '/' && *name_end != '\r') {
    name_end++;
  }
  node->tag = strndup(q, name_end - q);
  q = name_end;
  // Attributes: name="value" pairs.
  for (;;) {
    while (q < close && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) {
      q++;
    }
    const char* eq = memchr(q, '=', close - q);
    if (!eq || q >= close || *q == '/') {
      break;
    }
    const char* quote = eq + 1;
    if (quote >= close || (*quote != '"' && *quote != '\'')) {
      break;
    }
    const char* value_end = memchr(quote + 1, *quote, close - quote - 1);
    if (!value_end) {
      break;
    }
    const char* attr_end = eq;
    while (attr_end > q && (attr_end[-1] == ' ' || attr_end[-1] == '\t')) {
      attr_end--;
    }
    node->attributes = realloc(node->attributes,
                               sizeof(char*) * (node->attribute_count + 2) * 2);
    node->attributes[node->attribute_count * 2] = strndup(q, attr_end - q);
    node->attributes[node->attribute_count * 2 + 1] =
        decode(quote + 1, value_end);
    node->attribute_count++;
    q = value_end + 1;
  }
  r->p = close + 1;
  if (close[-1] == '/') {
    return node;
  }

  char* text = NULL;
  size_t text_len = 0;
  append(&text, &text_len, "", 0);
  while (r->p < r->end) {
    if (r->end - r->p >= 9 && !memcmp(r->p, "<![CDATA[", 9)) {
      const char* end = memmem(r->p + 9, r->end - r->p - 9, "]]>", 3);
      if (!end) {
        break;
      }
      append(&text, &text_len, r->p + 9, end - (r->p + 9));
      r->p = end + 3;
    } else if (r->end - r->p >= 4 && !memcmp(r->p, "<!--", 4)) {
      skip_misc(r);
    } else if (*r->p == '<' && r->p + 1 < r->end && r->p[1] == '/') {
      const char* end = memchr(r->p, '>', r->end - r->p);
      r->p = end ? end + 1 : r->end;
      break;
    } else if (*r->p == '<') {
      xml_node* child = parse_element(r);
      if (!child) {
        break;
      }
      node->children = realloc(node->children,
                               sizeof(xml_node*) * (node->child_count + 1));
      node->children[node->child_count++] = child;
    } else {
      const char* lt = memchr(r->p, '<', r->end - r->p);
      const char* stop = lt ? lt : r->end;
      char* decoded = decode(r->p, stop);
      append(&text, &text_len, decoded, strlen(decoded));
      free(decoded);
      r->p = stop;
    }
  }
  node->text = text;
  return node;
}

static void free_node(xml_node* node) {
  if (!node) {
    return;
  }
  for (int i = 0; i < node->child_count; i++) {
    free_node(node->children[i]);
  }
  for (int i = 0; i < node->attribute_count * 2; i++) {
    free(node->attributes[i]);
  }
  free(node->children);
  free(node->attributes);
  free(node->tag);
  free(node->text);
  free(node);
}

const char* xml_attribute(const xml_node* node, const char* name) {
  for (int i = 0; node && i < node->attribute_count; i++) {
    if (!strcmp(node->attributes[i * 2], name)) {
      return node->attributes[i * 2 + 1];
    }
  }
  return NULL;
}

xml_node* xml_named_child(const xml_node* node, const char* name) {
  for (int i = 0; node && i < node->child_count; i++) {
    const char* n = xml_attribute(node->children[i], "name");
    if (n && !strcmp(n, name)) {
      return node->children[i];
    }
  }
  return NULL;
}

// ---------------------------------------------------------------------------
// NIB objects

static void index_ids(hle_nib* nib, xml_node* node) {
  if (xml_attribute(node, "id") && strcmp(node->tag, "reference")) {
    nib->by_id = realloc(nib->by_id, sizeof(xml_node*) * (nib->id_count + 1));
    nib->by_id[nib->id_count++] = node;
  }
  for (int i = 0; i < node->child_count; i++) {
    index_ids(nib, node->children[i]);
  }
}

xml_node* hle_nib_resolve(hle_nib* nib, xml_node* node) {
  if (!node || strcmp(node->tag, "reference")) {
    return node;
  }
  const char* ref = xml_attribute(node, "idRef");
  for (int i = 0; ref && i < nib->id_count; i++) {
    if (!strcmp(xml_attribute(nib->by_id[i], "id"), ref)) {
      return nib->by_id[i];
    }
  }
  return NULL;
}

const char* hle_nib_property(const xml_node* object, const char* name) {
  xml_node* child = xml_named_child(object, name);
  return child ? child->text : NULL;
}

xml_node* hle_nib_object(hle_nib* nib, const xml_node* object,
                         const char* name) {
  return hle_nib_resolve(nib, xml_named_child(object, name));
}

xml_node* hle_nib_named(hle_nib* nib, const char* name) {
  // <object name="nameTable" class="NSDictionary">: a <string> key, then
  // the object it names.
  xml_node* table = NULL;
  for (int i = 0; i < nib->id_count && !table; i++) {
    table = xml_named_child(nib->by_id[i], "nameTable");
  }
  if (!table) {
    table = xml_named_child(nib->root, "nameTable");
  }
  for (int i = 0; table && i + 1 < table->child_count; i++) {
    xml_node* key = table->children[i];
    if (!strcmp(key->tag, "string") && key->text &&
        !strcmp(key->text, name)) {
      return hle_nib_resolve(nib, table->children[i + 1]);
    }
  }
  return NULL;
}

hle_nib* hle_nib_open(const char* name) {
  CFStringRef cf_name = cf_string_from_utf8(name, strlen(name));
  CFStringRef type = cf_string_from_utf8("nib", 3);
  CFURLRef url = CFBundleCopyResourceURL(CFBundleGetMainBundle(), cf_name,
                                         type, NULL);
  CFRelease(cf_name);
  CFRelease(type);
  if (!url) {
    return NULL;
  }
  char path[PATH_MAX];
  int ok = CFURLGetFileSystemRepresentation(url, 1, (UInt8*)path,
                                            sizeof(path) - 16);
  CFRelease(url);
  if (!ok) {
    return NULL;
  }
  strcat(path, "/objects.xib");
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
  if (!contents.data) {
    return NULL;
  }
  reader r = { contents.data, contents.data + contents.len };
  xml_node* root = parse_element(&r);
  cf_buf_free(&contents);
  if (!root) {
    fprintf(stderr, "hle: %s is not readable XML\n", path);
    return NULL;
  }
  hle_nib* nib = calloc(1, sizeof(*nib));
  nib->root = root;
  index_ids(nib, root);
  cf_trace("NIB %s: %d objects", path, nib->id_count);
  return nib;
}

void hle_nib_close(hle_nib* nib) {
  if (nib) {
    free_node(nib->root);
    free(nib->by_id);
    free(nib);
  }
}
