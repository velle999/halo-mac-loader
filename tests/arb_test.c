// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Checks hle/arb_program.c: an ALIAS of a binding, which Apple's ARB
// assembler took and other drivers refuse, becomes a declaration, and
// nothing else in the program changes.
//
//   make tests/arb_test && tests/arb_test [program files...]
//
// Program files named, such as the game's GameData/Shaders/vsh/*.vsh, are
// rewritten and checked too.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle/arb_program.h"

static int failures;

// Whether |out| is |in| with ALIAS keywords changed to OUTPUT or ATTRIB,
// and nothing else.
static int only_keywords_differ(const char* in, int32_t in_length,
                                const char* out, int32_t out_length) {
  int32_t i = 0;
  int32_t o = 0;
  while (i < in_length && o < out_length) {
    if (in_length - i >= 5 && !memcmp(in + i, "ALIAS", 5) &&
        out_length - o >= 6 &&
        (!memcmp(out + o, "OUTPUT", 6) || !memcmp(out + o, "ATTRIB", 6))) {
      i += 5;
      o += 6;
    } else if (in[i] == out[o]) {
      i++;
      o++;
    } else {
      return 0;
    }
  }
  return i == in_length && o == out_length;
}

// Rewrites |length| bytes of |in| and compares the result with |expected|,
// or, when that is NULL, checks that nothing changes.
static void check_rewrite(const char* what, const char* in, int32_t length,
                          const char* expected) {
  int32_t n = length;
  char* out = hle_arb_rewrite_aliases(in, &n);
  if (!expected) {
    if (out || n != length) {
      printf("FAIL: %s: the program was changed\n", what);
      failures++;
    }
  } else if (!out || n != (int32_t)strlen(expected) ||
             memcmp(out, expected, n) != 0 || out[n] != '\0') {
    printf("FAIL: %s: got\n%.*s\n", what, out ? (int)n : 0, out ? out : "");
    failures++;
  }
  free(out);
}

static char* read_file(const char* path, int32_t* length) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    return NULL;
  }
  char* data = NULL;
  if (fseek(f, 0, SEEK_END) == 0) {
    long size = ftell(f);
    rewind(f);
    data = size >= 0 ? malloc(size + 1) : NULL;
    if (data && fread(data, 1, size, f) == (size_t)size) {
      data[size] = '\0';
      *length = (int32_t)size;
    } else {
      free(data);
      data = NULL;
    }
  }
  fclose(f);
  return data;
}

int main(int argc, char** argv) {
  // As the game's programs are written: CRLF line ends and comments.
  static const char kProgram[] =
      "!!ARBvp1.0\r\n"
      "# ALIAS oFog = result.fogcoord;\r\n"
      "TEMP r0, r1;\r\n"
      "PARAM c[96] = { program.env[0..95] };\r\n"
      "ALIAS oPos = result.position;\r\n"
      "ALIAS\toD0=result.color.primary ;\r\n"
      "ATTRIB v0 = vertex.attrib[0]; ALIAS v1 = vertex.attrib[1];\r\n"
      "ALIAS scratch = r0;\r\n"
      "MOV oPos, v0;\r\n"
      "END\r\n";
  static const char kRewritten[] =
      "!!ARBvp1.0\r\n"
      "# ALIAS oFog = result.fogcoord;\r\n"
      "TEMP r0, r1;\r\n"
      "PARAM c[96] = { program.env[0..95] };\r\n"
      "OUTPUT oPos = result.position;\r\n"
      "OUTPUT\toD0=result.color.primary ;\r\n"
      "ATTRIB v0 = vertex.attrib[0]; ATTRIB v1 = vertex.attrib[1];\r\n"
      "ALIAS scratch = r0;\r\n"
      "MOV oPos, v0;\r\n"
      "END\r\n";
  check_rewrite("a vertex program", kProgram, sizeof(kProgram) - 1,
                kRewritten);

  static const char kVariable[] =
      "!!ARBvp1.0\nALIAS scratch = r0;\n"
      "MOV result.position, vertex.position;\nEND\n";
  check_rewrite("an ALIAS of a variable", kVariable, sizeof(kVariable) - 1,
                NULL);

  static const char kLongerName[] =
      "!!ARBvp1.0\nNOTALIAS oPos = result.position;\n";
  check_rewrite("ALIAS ending a longer name", kLongerName,
                sizeof(kLongerName) - 1, NULL);

  // The program ends at the length given, which cuts the second ALIAS off.
  static const char kLength[] =
      "ALIAS oPos = result.position;ALIAS oD0 = result.color";
  check_rewrite("the length given", kLength, 41,
                "OUTPUT oPos = result.position;ALIAS oD0 = ");

  int rewritten = 0;
  for (int i = 1; i < argc; i++) {
    int32_t length = 0;
    char* program = read_file(argv[i], &length);
    if (!program) {
      printf("FAIL: cannot read %s\n", argv[i]);
      failures++;
      continue;
    }
    int32_t n = length;
    char* out = hle_arb_rewrite_aliases(program, &n);
    if (out) {
      rewritten++;
      if (!only_keywords_differ(program, length, out, n)) {
        printf("FAIL: %s: more than ALIAS changed\n", argv[i]);
        failures++;
      }
      int32_t again = n;
      char* twice = hle_arb_rewrite_aliases(out, &again);
      if (twice) {
        printf("FAIL: %s: an ALIAS of a binding is left\n", argv[i]);
        failures++;
      }
      free(twice);
    }
    free(out);
    free(program);
  }
  if (argc > 1) {
    printf("%d of %d programs rewritten\n", rewritten, argc - 1);
  }

  if (failures == 0) {
    printf("all passed\n");
  }
  return failures != 0;
}
