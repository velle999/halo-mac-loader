// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// ARB programs written for Mac OS X's OpenGL, rewritten where another
// driver reads the ARB grammar more strictly.
//
// Apple's assembler lets ALIAS name a binding, as in
// "ALIAS oPos = result.position;", and every one of the game's vertex
// programs does. The ARB_vertex_program grammar only aliases variables
// already declared, so NVIDIA's driver refuses them. Declaring the binding
// means the same thing: "OUTPUT oPos = result.position;", or ATTRIB for a
// vertex binding.

#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>

#include "arb_program.h"

static int is_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int is_name_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '$';
}

char* hle_arb_rewrite_aliases(const char* text, int32_t* length) {
  static const char kAlias[] = "ALIAS";
  const int32_t n = *length;
  if (n <= 0 || !memmem(text, n, kAlias, sizeof(kAlias) - 1)) {
    return NULL;
  }
  // Each rewrite lengthens the text by one byte.
  char* out = malloc((size_t)n * 2 + 1);
  if (!out) {
    return NULL;
  }
  int32_t o = 0;
  int changed = 0;
  int statement_start = 1;
  int in_comment = 0;
  for (int32_t i = 0; i < n;) {
    char c = text[i];
    if (in_comment) {
      out[o++] = c;
      i++;
      if (c == '\n') {
        in_comment = 0;
        statement_start = 1;
      }
      continue;
    }
    if (c == '#') {
      in_comment = 1;
      out[o++] = c;
      i++;
      continue;
    }
    if (statement_start && n - i > 5 && !memcmp(text + i, kAlias, 5) &&
        is_space(text[i + 5])) {
      int32_t j = i + 5;
      while (j < n && is_space(text[j])) {
        j++;
      }
      while (j < n && is_name_char(text[j])) {
        j++;
      }
      while (j < n && is_space(text[j])) {
        j++;
      }
      const char* keyword = NULL;
      if (j < n && text[j] == '=') {
        j++;
        while (j < n && is_space(text[j])) {
          j++;
        }
        if (n - j >= 7 && !memcmp(text + j, "result.", 7)) {
          keyword = "OUTPUT";
        } else if (n - j >= 7 && !memcmp(text + j, "vertex.", 7)) {
          keyword = "ATTRIB";
        }
      }
      if (keyword) {
        memcpy(out + o, keyword, 6);
        o += 6;
        i += 5;
        changed = 1;
        statement_start = 0;
        continue;
      }
    }
    out[o++] = c;
    i++;
    if (c == ';' || c == '\n') {
      statement_start = 1;
    } else if (!is_space(c)) {
      statement_start = 0;
    }
  }
  if (!changed) {
    free(out);
    return NULL;
  }
  out[o] = '\0';
  *length = o;
  return out;
}
