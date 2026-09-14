// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Counts of the game's costly GL calls per frame: HLE_GL_STATS=1. See
// gl_stats.c.

#ifndef HLE_GL_STATS_H_
#define HLE_GL_STATS_H_

#include <stddef.h>

// |real|, the GL function |name| names, or, when HLE_GL_STATS is set and the
// function is one that is counted, a wrapper that counts calls to it.
void* hle_gl_stats_wrap(const char* name, void* real);

// Writes the counts per frame over the last |frames| frames to |out|, as
// text to append to the frame trace, and starts counting again. |out| is
// left empty when HLE_GL_STATS is not set.
void hle_gl_stats_take(char* out, size_t size, unsigned frames);

#endif  // HLE_GL_STATS_H_
