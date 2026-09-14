// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Screenshots on Print Screen. The next frame the game finishes is saved as
// it drew it, as a BMP file in HLE_SCREENSHOTS, or in ~/halo-screenshots.
// The game's own screenshots need a command-line switch its Mac startup
// never passes.
//
// A BMP holds its rows bottom to top, padded to four bytes, in blue, green,
// red order, which is exactly how glReadPixels gives GL_BGR with a pack
// alignment of 4.

#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <SDL2/SDL.h>

#include "gui.h"

enum {
  GL_BACK = 0x0405,
  GL_READ_BUFFER = 0x0C02,
  GL_PACK_ALIGNMENT = 0x0D05,
  GL_UNSIGNED_BYTE = 0x1401,
  GL_BGR = 0x80E0,
};

static int requested;

void hle_screenshot_request(void) {
  requested = 1;
}

static void put_u16(uint8_t* p, uint16_t v) {
  p[0] = v & 0xff;
  p[1] = v >> 8;
}

static void put_u32(uint8_t* p, uint32_t v) {
  put_u16(p, v & 0xffff);
  put_u16(p + 2, v >> 16);
}

// Where the next screenshot goes: a directory made if needed and a name for
// the time, numbered when one second holds several.
static int screenshot_path(char* path, size_t size) {
  const char* directory = getenv("HLE_SCREENSHOTS");
  char fallback[4096];
  if (!directory || !*directory) {
    const char* home = getenv("HOME");
    snprintf(fallback, sizeof(fallback), "%s/halo-screenshots",
             home ? home : ".");
    directory = fallback;
  }
  if (mkdir(directory, 0755) != 0 && errno != EEXIST) {
    fprintf(stderr, "hle: screenshot: cannot make %s: %m\n", directory);
    return 0;
  }
  time_t now = time(NULL);
  struct tm local;
  localtime_r(&now, &local);
  char stamp[32];
  strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
  struct stat st;
  for (int n = 0; n < 100; n++) {
    if (n == 0) {
      snprintf(path, size, "%s/halo-%s.bmp", directory, stamp);
    } else {
      snprintf(path, size, "%s/halo-%s-%d.bmp", directory, stamp, n);
    }
    if (stat(path, &st) != 0) {
      return 1;
    }
  }
  return 0;
}

void hle_screenshot_take(void* sdl_window) {
  // HLE_SCREENSHOT_FRAME=<n> asks for one at the nth frame, for runs with
  // nobody at the keyboard.
  static unsigned long frames;
  static long at = -1;
  if (at < 0) {
    const char* setting = getenv("HLE_SCREENSHOT_FRAME");
    at = setting && atol(setting) > 0 ? atol(setting) : 0;
  }
  if (++frames == (unsigned long)at) {
    requested = 1;
  }
  if (!requested) {
    return;
  }
  requested = 0;
  void (*get_integer)(uint32_t, int32_t*) =
      dlsym(RTLD_DEFAULT, "glGetIntegerv");
  void (*read_buffer)(uint32_t) = dlsym(RTLD_DEFAULT, "glReadBuffer");
  void (*pixel_store)(uint32_t, int32_t) =
      dlsym(RTLD_DEFAULT, "glPixelStorei");
  void (*read_pixels)(int32_t, int32_t, int32_t, int32_t, uint32_t,
                      uint32_t, void*) = dlsym(RTLD_DEFAULT, "glReadPixels");
  int width = 0;
  int height = 0;
  SDL_GL_GetDrawableSize(sdl_window, &width, &height);
  size_t row = ((size_t)width * 3 + 3) & ~(size_t)3;
  size_t pixels_size = row * (size_t)height;
  uint8_t* file = width > 0 && height > 0 ? malloc(54 + pixels_size) : NULL;
  char path[4200];
  if (!get_integer || !read_buffer || !pixel_store || !read_pixels || !file ||
      !screenshot_path(path, sizeof(path))) {
    free(file);
    return;
  }

  int32_t buffer = GL_BACK;
  int32_t alignment = 4;
  get_integer(GL_READ_BUFFER, &buffer);
  get_integer(GL_PACK_ALIGNMENT, &alignment);
  read_buffer(GL_BACK);
  pixel_store(GL_PACK_ALIGNMENT, 4);
  read_pixels(0, 0, width, height, GL_BGR, GL_UNSIGNED_BYTE, file + 54);
  pixel_store(GL_PACK_ALIGNMENT, alignment);
  read_buffer(buffer);

  memset(file, 0, 54);
  file[0] = 'B';
  file[1] = 'M';
  put_u32(file + 2, 54 + pixels_size);
  put_u32(file + 10, 54);
  put_u32(file + 14, 40);
  put_u32(file + 18, width);
  put_u32(file + 22, height);  // positive: the rows run bottom to top
  put_u16(file + 26, 1);
  put_u16(file + 28, 24);
  put_u32(file + 34, pixels_size);
  put_u32(file + 38, 2835);  // 72 dots an inch
  put_u32(file + 42, 2835);

  FILE* f = fopen(path, "wb");
  if (f && fwrite(file, 1, 54 + pixels_size, f) == 54 + pixels_size &&
      fclose(f) == 0) {
    fprintf(stderr, "hle: screenshot saved to %s\n", path);
  } else {
    if (f) {
      fclose(f);
    }
    fprintf(stderr, "hle: screenshot: cannot write %s: %m\n", path);
  }
  free(file);
}
