// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// ARB programs written for Mac OS X's OpenGL, rewritten for drivers that
// read the ARB grammar more strictly.

#ifndef HLE_ARB_PROGRAM_H_
#define HLE_ARB_PROGRAM_H_

#include <stdint.h>

// Declares each binding an ALIAS statement names in the |*length| bytes at
// |text|: OUTPUT for a result binding, ATTRIB for a vertex one. Returns the
// new program, NUL-terminated and allocated with malloc, and sets |*length|
// to its length; or returns NULL, leaving |*length| alone, when nothing
// changes. Comments are left as they are.
char* hle_arb_rewrite_aliases(const char* text, int32_t* length);

#endif  // HLE_ARB_PROGRAM_H_
