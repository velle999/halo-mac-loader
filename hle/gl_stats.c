// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Counts of the GL calls that cost the most, for finding where frames go.
//
// HLE_GL_STATS=1 puts a wrapper in front of the draw calls, texture uploads
// and copies, the calls that wait for the GPU, texture binds and program
// parameters as the dispatch table is filled, and aglSwapBuffers adds how
// many of each the game made per frame to its trace. The game makes its GL
// calls on one thread, so the counts are plain.

#include "gl_stats.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  uint64_t draws;
  uint64_t vertices;  // drawn by glDrawArrays or spanned by glDrawRangeElements
  uint64_t indices;
  uint64_t locks;
  uint64_t uploads;
  uint64_t texels;
  uint64_t compressed_bytes;
  uint64_t copies;
  uint64_t waits;
  uint64_t binds;
  uint64_t parameters;
} gl_counts;

static gl_counts counts;
static int enabled = -1;

static void (*real_draw_arrays)(uint32_t, int32_t, int32_t);
static void (*real_draw_elements)(uint32_t, int32_t, uint32_t, const void*);
static void (*real_draw_range_elements)(uint32_t, uint32_t, uint32_t, int32_t,
                                        uint32_t, const void*);
static void (*real_lock_arrays)(int32_t, int32_t);
static void (*real_tex_image_2d)(uint32_t, int32_t, int32_t, int32_t, int32_t,
                                 int32_t, uint32_t, uint32_t, const void*);
static void (*real_tex_sub_image_2d)(uint32_t, int32_t, int32_t, int32_t,
                                     int32_t, int32_t, uint32_t, uint32_t,
                                     const void*);
static void (*real_compressed_tex_image_2d)(uint32_t, int32_t, uint32_t,
                                            int32_t, int32_t, int32_t, int32_t,
                                            const void*);
static void (*real_compressed_tex_sub_image_2d)(uint32_t, int32_t, int32_t,
                                                int32_t, int32_t, int32_t,
                                                uint32_t, int32_t,
                                                const void*);
static void (*real_copy_tex_sub_image_2d)(uint32_t, int32_t, int32_t, int32_t,
                                          int32_t, int32_t, int32_t, int32_t);
static void (*real_read_pixels)(int32_t, int32_t, int32_t, int32_t, uint32_t,
                                uint32_t, void*);
static void (*real_finish)(void);
static void (*real_flush)(void);
static void (*real_bind_texture)(uint32_t, uint32_t);
static void (*real_program_env_4fv)(uint32_t, uint32_t, const float*);
static void (*real_program_local_4fv)(uint32_t, uint32_t, const float*);
static void (*real_program_env_4f)(uint32_t, uint32_t, float, float, float,
                                   float);
static void (*real_program_local_4f)(uint32_t, uint32_t, float, float, float,
                                     float);

static void draw_arrays(uint32_t mode, int32_t first, int32_t count) {
  counts.draws++;
  counts.vertices += count;
  real_draw_arrays(mode, first, count);
}

static void draw_elements(uint32_t mode, int32_t count, uint32_t type,
                          const void* indices) {
  counts.draws++;
  counts.indices += count;
  real_draw_elements(mode, count, type, indices);
}

static void draw_range_elements(uint32_t mode, uint32_t start, uint32_t end,
                                int32_t count, uint32_t type,
                                const void* indices) {
  counts.draws++;
  counts.indices += count;
  counts.vertices += end >= start ? end - start + 1 : 0;
  real_draw_range_elements(mode, start, end, count, type, indices);
}

static void lock_arrays(int32_t first, int32_t count) {
  counts.locks++;
  real_lock_arrays(first, count);
}

static void tex_image_2d(uint32_t target, int32_t level, int32_t internal,
                         int32_t width, int32_t height, int32_t border,
                         uint32_t format, uint32_t type, const void* pixels) {
  counts.uploads++;
  counts.texels += (uint64_t)width * height;
  real_tex_image_2d(target, level, internal, width, height, border, format,
                    type, pixels);
}

static void tex_sub_image_2d(uint32_t target, int32_t level, int32_t x,
                             int32_t y, int32_t width, int32_t height,
                             uint32_t format, uint32_t type,
                             const void* pixels) {
  counts.uploads++;
  counts.texels += (uint64_t)width * height;
  real_tex_sub_image_2d(target, level, x, y, width, height, format, type,
                        pixels);
}

static void compressed_tex_image_2d(uint32_t target, int32_t level,
                                    uint32_t internal, int32_t width,
                                    int32_t height, int32_t border,
                                    int32_t size, const void* data) {
  counts.uploads++;
  counts.compressed_bytes += size;
  real_compressed_tex_image_2d(target, level, internal, width, height, border,
                               size, data);
}

static void compressed_tex_sub_image_2d(uint32_t target, int32_t level,
                                        int32_t x, int32_t y, int32_t width,
                                        int32_t height, uint32_t format,
                                        int32_t size, const void* data) {
  counts.uploads++;
  counts.compressed_bytes += size;
  real_compressed_tex_sub_image_2d(target, level, x, y, width, height, format,
                                   size, data);
}

static void copy_tex_sub_image_2d(uint32_t target, int32_t level,
                                  int32_t xoffset, int32_t yoffset, int32_t x,
                                  int32_t y, int32_t width, int32_t height) {
  counts.copies++;
  real_copy_tex_sub_image_2d(target, level, xoffset, yoffset, x, y, width,
                             height);
}

static void read_pixels(int32_t x, int32_t y, int32_t width, int32_t height,
                        uint32_t format, uint32_t type, void* pixels) {
  counts.waits++;
  real_read_pixels(x, y, width, height, format, type, pixels);
}

static void finish(void) {
  counts.waits++;
  real_finish();
}

static void flush(void) {
  counts.waits++;
  real_flush();
}

static void bind_texture(uint32_t target, uint32_t texture) {
  counts.binds++;
  real_bind_texture(target, texture);
}

static void program_env_4fv(uint32_t target, uint32_t index,
                            const float* values) {
  counts.parameters++;
  real_program_env_4fv(target, index, values);
}

static void program_local_4fv(uint32_t target, uint32_t index,
                              const float* values) {
  counts.parameters++;
  real_program_local_4fv(target, index, values);
}

static void program_env_4f(uint32_t target, uint32_t index, float x, float y,
                           float z, float w) {
  counts.parameters++;
  real_program_env_4f(target, index, x, y, z, w);
}

static void program_local_4f(uint32_t target, uint32_t index, float x,
                             float y, float z, float w) {
  counts.parameters++;
  real_program_local_4f(target, index, x, y, z, w);
}

typedef struct {
  const char* name;
  void* wrapper;
  void** real;
} gl_wrap;

static const gl_wrap wraps[] = {
  { "glDrawArrays", draw_arrays, (void**)&real_draw_arrays },
  { "glDrawElements", draw_elements, (void**)&real_draw_elements },
  { "glDrawRangeElements", draw_range_elements,
    (void**)&real_draw_range_elements },
  { "glDrawRangeElementsEXT", draw_range_elements,
    (void**)&real_draw_range_elements },
  { "glLockArraysEXT", lock_arrays, (void**)&real_lock_arrays },
  { "glTexImage2D", tex_image_2d, (void**)&real_tex_image_2d },
  { "glTexSubImage2D", tex_sub_image_2d, (void**)&real_tex_sub_image_2d },
  { "glCompressedTexImage2D", compressed_tex_image_2d,
    (void**)&real_compressed_tex_image_2d },
  { "glCompressedTexImage2DARB", compressed_tex_image_2d,
    (void**)&real_compressed_tex_image_2d },
  { "glCompressedTexSubImage2D", compressed_tex_sub_image_2d,
    (void**)&real_compressed_tex_sub_image_2d },
  { "glCompressedTexSubImage2DARB", compressed_tex_sub_image_2d,
    (void**)&real_compressed_tex_sub_image_2d },
  { "glCopyTexSubImage2D", copy_tex_sub_image_2d,
    (void**)&real_copy_tex_sub_image_2d },
  { "glReadPixels", read_pixels, (void**)&real_read_pixels },
  { "glFinish", finish, (void**)&real_finish },
  { "glFlush", flush, (void**)&real_flush },
  { "glBindTexture", bind_texture, (void**)&real_bind_texture },
  { "glProgramEnvParameter4fvARB", program_env_4fv,
    (void**)&real_program_env_4fv },
  { "glProgramLocalParameter4fvARB", program_local_4fv,
    (void**)&real_program_local_4fv },
  { "glProgramEnvParameter4fARB", program_env_4f,
    (void**)&real_program_env_4f },
  { "glProgramLocalParameter4fARB", program_local_4f,
    (void**)&real_program_local_4f },
};

void* hle_gl_stats_wrap(const char* name, void* real) {
  if (enabled < 0) {
    const char* setting = getenv("HLE_GL_STATS");
    enabled = setting && *setting && strcmp(setting, "0") != 0;
  }
  if (!enabled || !real) {
    return real;
  }
  for (size_t i = 0; i < sizeof(wraps) / sizeof(wraps[0]); i++) {
    if (!strcmp(name, wraps[i].name)) {
      *wraps[i].real = real;
      return wraps[i].wrapper;
    }
  }
  return real;
}

void hle_gl_stats_take(char* out, size_t size, unsigned frames) {
  if (size) {
    out[0] = '\0';
  }
  if (enabled > 0 && frames) {
    double n = frames;
    snprintf(out, size,
             "; GL per frame: %.0f draws (%.0f vertices, %.0f indices), "
             "%.1f locks, %.1f texture uploads (%.0f texels, %.0f compressed "
             "bytes), %.1f copies, %.1f waits, %.0f binds, %.0f program "
             "parameters",
             counts.draws / n, counts.vertices / n, counts.indices / n,
             counts.locks / n, counts.uploads / n, counts.texels / n,
             counts.compressed_bytes / n, counts.copies / n, counts.waits / n,
             counts.binds / n, counts.parameters / n);
  }
  memset(&counts, 0, sizeof(counts));
}
