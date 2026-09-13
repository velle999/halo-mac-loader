// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// OpenGL contexts: CGL and AGL over SDL's.
//
// A Mac context object begins with its renderer pointer and the dispatch
// table code built with aglMacro.h calls through, so contexts here begin
// the same way, the table holding gl_dispatch.c's thunks. Each SDL context
// is created against a hidden window of its pixel format and moves to the
// SDL window of the Carbon window, or of the screen, the game attaches.
//
// The renderer described is one accelerated NVIDIA renderer. HLE_VRAM_MB
// sets the video memory it claims (256 by default), and HLE_WINDOWED=1
// keeps a full-screen context in a window of the same size.

#define _GNU_SOURCE

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "gl_dispatch.h"
#include "gui.h"

enum {
  kMagicPixelFormat = 'pixf',
  kMagicContext = 'aglc',
  kMagicRendererInfo = 'rinf',
  kRendererGeForceFX = 0x00022400,
};

// Attribute names, shared by AGL and CGL.
enum {
  AGL_NONE = 0,
  AGL_ALL_RENDERERS = 1,
  AGL_BUFFER_SIZE = 2,
  AGL_LEVEL = 3,
  AGL_RGBA = 4,
  AGL_DOUBLEBUFFER = 5,
  AGL_STEREO = 6,
  AGL_AUX_BUFFERS = 7,
  AGL_RED_SIZE = 8,
  AGL_GREEN_SIZE = 9,
  AGL_BLUE_SIZE = 10,
  AGL_ALPHA_SIZE = 11,
  AGL_DEPTH_SIZE = 12,
  AGL_STENCIL_SIZE = 13,
  AGL_ACCUM_RED_SIZE = 14,
  AGL_ACCUM_GREEN_SIZE = 15,
  AGL_ACCUM_BLUE_SIZE = 16,
  AGL_ACCUM_ALPHA_SIZE = 17,
  AGL_PIXEL_SIZE = 50,
  AGL_OFFSCREEN = 53,
  AGL_FULLSCREEN = 54,
  AGL_SAMPLE_BUFFERS_ARB = 55,
  AGL_SAMPLES_ARB = 56,
  AGL_RENDERER_ID = 70,
  AGL_ACCELERATED = 73,
  AGL_WINDOW = 80,
  AGL_VIRTUAL_SCREEN = 82,
  kCGLPFADisplayMask = 84,
  AGL_PBUFFER = 90,
  AGL_BUFFER_MODES = 100,
  AGL_COLOR_MODES = 103,
  AGL_VIDEO_MEMORY = 120,
  AGL_TEXTURE_MEMORY = 121,
  AGL_RENDERER_COUNT = 128,
  AGL_SWAP_INTERVAL = 222,
};

enum {
  AGL_NO_ERROR = 0,
  AGL_BAD_ATTRIBUTE = 10000,
  AGL_BAD_PROPERTY = 10001,
  AGL_BAD_PIXELFMT = 10002,
  AGL_BAD_RENDINFO = 10003,
  AGL_BAD_CONTEXT = 10004,
  AGL_BAD_DRAWABLE = 10005,
  AGL_BAD_VALUE = 10008,
  AGL_BAD_ALLOC = 10016,
};

typedef struct {
  uint32_t magic;
  int double_buffer;
  int depth;
  int stencil;
  int alpha;
  int color;
  int samples;
  int fullscreen;
  int pbuffer;
  int plain;          // the requested visual was not there; SDL's default is
  SDL_Window* probe;  // hidden, for creating contexts of this format
} hle_pixel_format;

typedef struct hle_gl_context {
  void* rend;
  void* disp[686];
  void* priv;
  void* stak;
  uint32_t magic;
  SDL_GLContext gl;
  SDL_Window* window;  // where it draws now
  hle_pixel_format* format;
  void* drawable;
  int fullscreen;
} hle_gl_context;

typedef struct {
  uint32_t magic;
  int count;
} hle_renderer_info;

static __thread hle_gl_context* current;
static __thread int agl_error;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int dispatch_resolved;

// ---------------------------------------------------------------------------
// The extension string
//
// The game copies GL_EXTENSIONS into a 4096-byte buffer, and a later driver
// lists far more than a 2006 Mac did. It is given the extensions whose names
// its executable contains, the only ones it can ask about, plus
// EXT_texture_rectangle where the driver has ARB_texture_rectangle, which
// uses the same enumerants.

enum {
  GL_EXTENSIONS = 0x1F03,
  kMaxExtensionString = 4000,
};

static int is_name_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '_';
}

// Whether |text| holds the |length| bytes at |name| as a whole word.
static int has_word(const char* text, size_t size, const char* name,
                    size_t length) {
  const char* end = text + size;
  for (const char* p = text; p < end &&
       (p = memmem(p, end - p, name, length)); p += length) {
    int starts = p == text || !is_name_char(p[-1]);
    int ends = p + length == end || !is_name_char(p[length]);
    if (starts && ends) {
      return 1;
    }
  }
  return 0;
}

static char* read_executable(size_t* size) {
  FILE* f = fopen(__darwin_executable_path, "rb");
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
  *size = contents.len;
  return contents.data;
}

static const char* game_extensions(const char* all) {
  static pthread_mutex_t extensions_lock = PTHREAD_MUTEX_INITIALIZER;
  static char* extensions;
  pthread_mutex_lock(&extensions_lock);
  if (!extensions) {
    size_t image_size = 0;
    char* image = read_executable(&image_size);
    cf_buf out = { 0 };
    cf_buf_appends(&out, "");
    for (const char* p = all; *p;) {
      while (*p == ' ') {
        p++;
      }
      const char* end = p;
      while (*end && *end != ' ') {
        end++;
      }
      size_t length = end - p;
      if (length && (!image || has_word(image, image_size, p, length)) &&
          out.len + length + 1 < kMaxExtensionString) {
        cf_buf_append(&out, p, length);
        cf_buf_append(&out, " ", 1);
      }
      p = end;
    }
    static const char kArbRectangle[] = "GL_ARB_texture_rectangle";
    static const char kExtRectangle[] = "GL_EXT_texture_rectangle";
    size_t all_size = strlen(all);
    if (image &&
        has_word(all, all_size, kArbRectangle, sizeof(kArbRectangle) - 1) &&
        !has_word(all, all_size, kExtRectangle, sizeof(kExtRectangle) - 1) &&
        has_word(image, image_size, kExtRectangle, sizeof(kExtRectangle) - 1)) {
      cf_buf_appends(&out, kExtRectangle);
      cf_buf_append(&out, " ", 1);
    }
    free(image);
    extensions = out.data;
    cf_trace("GL_EXTENSIONS: %zu bytes of the driver's %zu, the extensions "
             "the game names", strlen(extensions), all_size);
  }
  pthread_mutex_unlock(&extensions_lock);
  return extensions;
}

// The game's glGetString, through rename.tab and the dispatch table.
const uint8_t* __darwin_glGetString(uint32_t name) {
  static const uint8_t* (*real_get_string)(uint32_t);
  if (!real_get_string) {
    real_get_string = dlsym(RTLD_DEFAULT, "glGetString");
  }
  const uint8_t* s = real_get_string ? real_get_string(name) : NULL;
  if (s && name == GL_EXTENSIONS) {
    return (const uint8_t*)game_extensions((const char*)s);
  }
  return s;
}

static void* lookup_gl(const char* name) {
  if (!strcmp(name, "glGetString")) {
    return __darwin_glGetString;
  }
  return dlsym(RTLD_DEFAULT, name);
}

// ---------------------------------------------------------------------------
// The renderer

static int vram_bytes(void) {
  const char* mb = getenv("HLE_VRAM_MB");
  long n = mb ? strtol(mb, NULL, 10) : 256;
  if (n <= 0 || n > 2047) {
    n = 256;
  }
  return (int)(n << 20);
}

static int describe_renderer(int property, int32_t* value) {
  switch (property) {
    case AGL_RENDERER_ID: *value = kRendererGeForceFX; break;
    case AGL_ACCELERATED: *value = 1; break;
    case 75: *value = 0; break;           // robust
    case 76: *value = 1; break;           // backing store
    case 78: *value = 1; break;           // MP safe
    case AGL_WINDOW: *value = 1; break;
    case 81: *value = 0; break;           // multiscreen
    case 83: *value = 1; break;           // compliant
    case kCGLPFADisplayMask: *value = 1; break;
    case AGL_OFFSCREEN: *value = 0; break;
    case AGL_FULLSCREEN: *value = 1; break;
    case AGL_BUFFER_MODES: *value = 0x1 | 0x4 | 0x8; break;
    case 101: case 102: *value = 0; break;  // min and max level
    case AGL_COLOR_MODES: *value = 0x00004000 | 0x00008000; break;
    case 104: *value = 0x00004000 | 0x00008000; break;  // accum modes
    case 105: *value = 0x1 | 0x400 | 0x800; break;      // depth 0, 16, 24
    case 106: *value = 0x1 | 0x80; break;               // stencil 0, 8
    case 107: *value = 0; break;          // max aux buffers
    case 108: *value = 1; break;          // max sample buffers
    case 109: *value = 4; break;          // max samples
    case 110: *value = 0x2; break;        // multisample
    case 111: *value = 1; break;          // sample alpha
    case AGL_VIDEO_MEMORY:
    case AGL_TEXTURE_MEMORY: *value = vram_bytes(); break;
    case 122: case 123: *value = 1; break;  // GPU vertex, fragment processing
    case AGL_RENDERER_COUNT: *value = 1; break;
    default: return 0;
  }
  return 1;
}

static hle_renderer_info* new_renderer_info(void) {
  hle_renderer_info* info = calloc(1, sizeof(*info));
  info->magic = kMagicRendererInfo;
  info->count = 1;
  return info;
}

static hle_renderer_info* renderer_info_of(void* ref) {
  hle_renderer_info* info = ref;
  return info && info->magic == kMagicRendererInfo ? info : NULL;
}

// ---------------------------------------------------------------------------
// Pixel formats

static int takes_value(int attribute, int cgl) {
  switch (attribute) {
    case AGL_BUFFER_SIZE: case AGL_LEVEL: case AGL_AUX_BUFFERS:
    case AGL_RED_SIZE: case AGL_GREEN_SIZE: case AGL_BLUE_SIZE:
    case AGL_ALPHA_SIZE: case AGL_DEPTH_SIZE: case AGL_STENCIL_SIZE:
    case AGL_ACCUM_RED_SIZE: case AGL_ACCUM_GREEN_SIZE:
    case AGL_ACCUM_BLUE_SIZE: case AGL_ACCUM_ALPHA_SIZE: case AGL_PIXEL_SIZE:
    case AGL_SAMPLE_BUFFERS_ARB: case AGL_SAMPLES_ARB: case AGL_RENDERER_ID:
    case AGL_VIRTUAL_SCREEN:
      return 1;
    case kCGLPFADisplayMask:
      return cgl;
    default:
      return 0;
  }
}

// CGL's color size counts all channels where AGL's counts one.
static hle_pixel_format* parse_format(const int32_t* attributes, int cgl) {
  hle_pixel_format* f = calloc(1, sizeof(*f));
  f->magic = kMagicPixelFormat;
  f->color = 24;
  int sample_buffers = 0;
  for (const int32_t* a = attributes; a && *a != AGL_NONE; a++) {
    int value = takes_value(*a, cgl) ? a[1] : 0;
    switch (*a) {
      case AGL_DOUBLEBUFFER: f->double_buffer = 1; break;
      case AGL_DEPTH_SIZE: f->depth = value; break;
      case AGL_STENCIL_SIZE: f->stencil = value; break;
      case AGL_ALPHA_SIZE: f->alpha = value; break;
      case AGL_RED_SIZE:
        f->color = cgl ? value : value * 3;
        break;
      case AGL_BUFFER_SIZE: f->color = value; break;
      case AGL_SAMPLE_BUFFERS_ARB: sample_buffers = value; break;
      case AGL_SAMPLES_ARB: f->samples = value; break;
      case AGL_FULLSCREEN: f->fullscreen = 1; break;
      case AGL_PBUFFER: f->pbuffer = 1; break;
    }
    if (takes_value(*a, cgl)) {
      a++;
    }
  }
  if (!sample_buffers) {
    f->samples = 0;
  }
  return f;
}

static hle_pixel_format* format_of(void* ref) {
  hle_pixel_format* f = ref;
  return f && f->magic == kMagicPixelFormat ? f : NULL;
}

// The visual every window and context of a format is made with. A GLX
// server need not offer single-buffered or depthless visuals, and a larger
// one serves the same drawing, so those are always requested. When even
// that fails, |f->plain| falls back to SDL's defaults.
static void apply_format(const hle_pixel_format* f) {
  SDL_GL_ResetAttributes();
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  if (f->plain) {
    return;
  }
  SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
  SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, f->alpha ? 8 : 0);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, f->depth > 0 && f->depth <= 16 ? 16
                                                                         : 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, f->stencil ? 8 : 0);
  SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, f->samples ? 1 : 0);
  SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, f->samples);
}

static void destroy_format(hle_pixel_format* f) {
  if (f->probe) {
    SDL_DestroyWindow(f->probe);
  }
  f->magic = 0;
  free(f);
}

// ---------------------------------------------------------------------------
// Contexts

static hle_gl_context* context_of(void* ref) {
  hle_gl_context* c = ref;
  return c && c->magic == kMagicContext ? c : NULL;
}

static void make_current(hle_gl_context* c) {
  if (c) {
    SDL_GL_MakeCurrent(c->window, c->gl);
  } else {
    SDL_GL_MakeCurrent(NULL, NULL);
  }
  current = c;
}

static hle_gl_context* create_context(hle_pixel_format* f,
                                      hle_gl_context* share) {
  if (!hle_sdl_video()) {
    agl_error = AGL_BAD_CONTEXT;
    return NULL;
  }
  pthread_mutex_lock(&lock);
  if (!dispatch_resolved) {
    int missing = hle_gl_resolve(lookup_gl);
    cf_trace("GL dispatch: %d of %d entries have no GL function", missing,
             hle_gl_thunk_count);
    dispatch_resolved = 1;
  }
  pthread_mutex_unlock(&lock);

  apply_format(f);
  if (!f->probe) {
    f->probe = SDL_CreateWindow("", 0, 0, 16, 16,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!f->probe && !f->plain) {
      fprintf(stderr, "hle: GL visual (depth %d, stencil %d, alpha %d, "
              "%d samples): %s; trying SDL's default\n", f->depth, f->stencil,
              f->alpha, f->samples, SDL_GetError());
      f->plain = 1;
      apply_format(f);
      f->probe = SDL_CreateWindow("", 0, 0, 16, 16,
                                  SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    }
    if (!f->probe) {
      fprintf(stderr, "hle: GL window: %s\n", SDL_GetError());
      agl_error = AGL_BAD_PIXELFMT;
      return NULL;
    }
  }
  hle_gl_context* previous = current;
  if (share) {
    SDL_GL_MakeCurrent(share->window, share->gl);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
  }
  SDL_GLContext gl = SDL_GL_CreateContext(f->probe);
  SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 0);
  // Creating a context makes it current; the game did not ask for that.
  make_current(previous);
  if (!gl) {
    fprintf(stderr, "hle: GL context: %s\n", SDL_GetError());
    agl_error = AGL_BAD_CONTEXT;
    return NULL;
  }
  hle_gl_context* c = calloc(1, sizeof(*c));
  c->rend = c;
  memcpy(c->disp, hle_gl_thunks, sizeof(void*) * hle_gl_thunk_count);
  c->magic = kMagicContext;
  c->gl = gl;
  c->window = f->probe;
  c->format = f;
  return c;
}

static void destroy_context(hle_gl_context* c) {
  if (current == c) {
    make_current(NULL);
  }
  SDL_GL_DeleteContext(c->gl);
  c->magic = 0;
  free(c);
}

// Moves |c| to |window|, keeping it current if it was.
static void move_to(hle_gl_context* c, SDL_Window* window) {
  c->window = window ? window : c->format->probe;
  if (current == c) {
    make_current(c);
  }
}

// ---------------------------------------------------------------------------
// AGL

void aglGetVersion(int32_t* major, int32_t* minor) {
  if (major) {
    *major = 3;
  }
  if (minor) {
    *minor = 0;
  }
}

uint32_t aglGetError(void) {
  int err = agl_error;
  agl_error = AGL_NO_ERROR;
  return err;
}

void* aglChoosePixelFormat(const void* devices, int32_t count,
                           const int32_t* attributes) {
  return parse_format(attributes, 0);
}

void aglDestroyPixelFormat(void* pix) {
  hle_pixel_format* f = format_of(pix);
  if (f) {
    destroy_format(f);
  }
}

void* aglNextPixelFormat(void* pix) {
  return NULL;
}

Boolean aglDescribePixelFormat(void* pix, int32_t attribute, int32_t* value) {
  hle_pixel_format* f = format_of(pix);
  if (!f || !value) {
    agl_error = AGL_BAD_PIXELFMT;
    return 0;
  }
  switch (attribute) {
    case AGL_DOUBLEBUFFER: *value = f->double_buffer; break;
    case AGL_DEPTH_SIZE: *value = f->depth; break;
    case AGL_STENCIL_SIZE: *value = f->stencil; break;
    case AGL_ALPHA_SIZE: *value = f->alpha; break;
    case AGL_SAMPLES_ARB: *value = f->samples; break;
    case AGL_SAMPLE_BUFFERS_ARB: *value = f->samples ? 1 : 0; break;
    case AGL_RGBA: *value = 1; break;
    case AGL_PIXEL_SIZE: *value = 32; break;
    default:
      if (!describe_renderer(attribute, value)) {
        *value = 0;
      }
  }
  return 1;
}

void* aglQueryRendererInfo(const void* devices, int32_t count) {
  return new_renderer_info();
}

void* aglNextRendererInfo(void* rend) {
  return NULL;
}

void aglDestroyRendererInfo(void* rend) {
  hle_renderer_info* info = renderer_info_of(rend);
  if (info) {
    info->magic = 0;
    free(info);
  }
}

Boolean aglDescribeRenderer(void* rend, int32_t property, int32_t* value) {
  if (!renderer_info_of(rend) || !value) {
    agl_error = AGL_BAD_RENDINFO;
    return 0;
  }
  if (!describe_renderer(property, value)) {
    agl_error = AGL_BAD_PROPERTY;
    return 0;
  }
  return 1;
}

// The game destroys a pixel format as soon as it has a context from it, so
// a context keeps a copy of its own, with its own hidden window.
static hle_pixel_format* copy_format(const hle_pixel_format* f) {
  hle_pixel_format* own = malloc(sizeof(*own));
  *own = *f;
  own->probe = NULL;
  return own;
}

void* aglCreateContext(void* pix, void* share) {
  hle_pixel_format* f = format_of(pix);
  if (!f) {
    agl_error = AGL_BAD_PIXELFMT;
    return NULL;
  }
  hle_pixel_format* own = copy_format(f);
  hle_gl_context* c = create_context(own, context_of(share));
  if (!c) {
    destroy_format(own);
  }
  return c;
}

Boolean aglDestroyContext(void* ctx) {
  hle_gl_context* c = context_of(ctx);
  if (!c) {
    agl_error = AGL_BAD_CONTEXT;
    return 0;
  }
  hle_pixel_format* f = c->format;
  destroy_context(c);
  destroy_format(f);
  return 1;
}

Boolean aglSetCurrentContext(void* ctx) {
  hle_gl_context* c = context_of(ctx);
  if (ctx && !c) {
    agl_error = AGL_BAD_CONTEXT;
    return 0;
  }
  make_current(c);
  return 1;
}

void* aglGetCurrentContext(void) {
  return current;
}

Boolean aglSetDrawable(void* ctx, void* drawable) {
  hle_gl_context* c = context_of(ctx);
  if (!c) {
    agl_error = AGL_BAD_CONTEXT;
    return 0;
  }
  c->drawable = drawable;
  c->fullscreen = 0;
  if (!drawable) {
    move_to(c, NULL);
    return 1;
  }
  hle_port* port = drawable;
  hle_window* window = port->magic == kHleMagicPort ? port->window : NULL;
  if (!window) {
    agl_error = AGL_BAD_DRAWABLE;
    return 0;
  }
  apply_format(c->format);
  SDL_Window* sdl = hle_sdl_window_for(
      window, hle_window_wants_fullscreen(window),
      window->content.right - window->content.left,
      window->content.bottom - window->content.top);
  if (!sdl) {
    agl_error = AGL_BAD_DRAWABLE;
    return 0;
  }
  move_to(c, sdl);
  return 1;
}

void* aglGetDrawable(void* ctx) {
  hle_gl_context* c = context_of(ctx);
  return c ? c->drawable : NULL;
}

Boolean aglSetFullScreen(void* ctx, int32_t width, int32_t height,
                         int32_t frequency, int32_t device) {
  hle_gl_context* c = context_of(ctx);
  if (!c) {
    agl_error = AGL_BAD_CONTEXT;
    return 0;
  }
  const char* windowed = getenv("HLE_WINDOWED");
  int fullscreen = !(windowed && *windowed == '1');
  apply_format(c->format);
  SDL_Window* sdl = hle_sdl_window_for(NULL, fullscreen, width, height);
  if (!sdl) {
    agl_error = AGL_BAD_ALLOC;
    return 0;
  }
  cf_trace("aglSetFullScreen(%dx%d@%d)%s", width, height, frequency,
           fullscreen ? "" : " in a window");
  c->drawable = NULL;
  c->fullscreen = 1;
  move_to(c, sdl);
  return 1;
}

Boolean aglUpdateContext(void* ctx) {
  return context_of(ctx) != NULL;
}

void aglSwapBuffers(void* ctx) {
  hle_gl_context* c = context_of(ctx);
  if (c && c->window != c->format->probe) {
    SDL_GL_SwapWindow(c->window);
  }
}

Boolean aglSetInteger(void* ctx, uint32_t name, const int32_t* params) {
  hle_gl_context* c = context_of(ctx);
  if (!c) {
    agl_error = AGL_BAD_CONTEXT;
    return 0;
  }
  if (name == AGL_SWAP_INTERVAL && params) {
    hle_gl_context* previous = current;
    make_current(c);
    SDL_GL_SetSwapInterval(params[0]);
    make_current(previous);
  }
  return 1;
}

Boolean aglGetInteger(void* ctx, uint32_t name, int32_t* params) {
  hle_gl_context* c = context_of(ctx);
  if (!c || !params) {
    agl_error = AGL_BAD_CONTEXT;
    return 0;
  }
  params[0] = name == AGL_SWAP_INTERVAL ? SDL_GL_GetSwapInterval() : 0;
  return 1;
}

Boolean aglEnable(void* ctx, uint32_t name) {
  return context_of(ctx) != NULL;
}

Boolean aglDisable(void* ctx, uint32_t name) {
  return context_of(ctx) != NULL;
}

int32_t aglGetVirtualScreen(void* ctx) {
  return 0;
}

// Pbuffers are not here yet; the game is told there is no room for one.
Boolean aglCreatePBuffer(int32_t width, int32_t height, uint32_t target,
                         uint32_t internal_format, int32_t max_level,
                         void** pbuffer) {
  cf_warn_once("aglCreatePBuffer: pbuffers are not supported");
  if (pbuffer) {
    *pbuffer = NULL;
  }
  agl_error = AGL_BAD_ALLOC;
  return 0;
}

Boolean aglDestroyPBuffer(void* pbuffer) {
  return 1;
}

Boolean aglSetPBuffer(void* ctx, void* pbuffer, int32_t face, int32_t level,
                      int32_t screen) {
  agl_error = AGL_BAD_VALUE;
  return 0;
}

Boolean aglTexImagePBuffer(void* ctx, void* pbuffer, int32_t source) {
  agl_error = AGL_BAD_VALUE;
  return 0;
}

// ---------------------------------------------------------------------------
// CGL

enum {
  kCGLNoError = 0,
  kCGLBadAttribute = 10000,
  kCGLBadProperty = 10001,
  kCGLBadPixelFormat = 10002,
  kCGLBadRendererInfo = 10003,
  kCGLBadContext = 10004,
};

int CGLChoosePixelFormat(const int32_t* attributes, void** pix,
                         int32_t* count) {
  if (!pix) {
    return kCGLBadAttribute;
  }
  *pix = parse_format(attributes, 1);
  if (count) {
    *count = 1;
  }
  return kCGLNoError;
}

int CGLDestroyPixelFormat(void* pix) {
  hle_pixel_format* f = format_of(pix);
  if (!f) {
    return kCGLBadPixelFormat;
  }
  destroy_format(f);
  return kCGLNoError;
}

int CGLDescribePixelFormat(void* pix, int32_t index, int32_t attribute,
                           int32_t* value) {
  if (!format_of(pix)) {
    return kCGLBadPixelFormat;
  }
  if (attribute == 128) {  // kCGLPFAVirtualScreenCount
    *value = 1;
    return kCGLNoError;
  }
  aglDescribePixelFormat(pix, attribute, value);
  return kCGLNoError;
}

int CGLQueryRendererInfo(uint32_t display_mask, void** rend, int32_t* count) {
  if (!rend) {
    return kCGLBadRendererInfo;
  }
  *rend = new_renderer_info();
  if (count) {
    *count = 1;
  }
  return kCGLNoError;
}

int CGLDescribeRenderer(void* rend, int32_t index, int32_t property,
                        int32_t* value) {
  hle_renderer_info* info = renderer_info_of(rend);
  if (!info || index < 0 || index >= info->count) {
    return kCGLBadRendererInfo;
  }
  return value && describe_renderer(property, value) ? kCGLNoError
                                                     : kCGLBadProperty;
}

int CGLDestroyRendererInfo(void* rend) {
  aglDestroyRendererInfo(rend);
  return kCGLNoError;
}

int CGLCreateContext(void* pix, void* share, void** ctx) {
  hle_pixel_format* f = format_of(pix);
  if (!f || !ctx) {
    return kCGLBadPixelFormat;
  }
  // The context may outlive the format it came from.
  hle_pixel_format* own = malloc(sizeof(*own));
  *own = *f;
  own->probe = NULL;
  *ctx = create_context(own, context_of(share));
  if (!*ctx) {
    free(own);
    return kCGLBadContext;
  }
  return kCGLNoError;
}

int CGLDestroyContext(void* ctx) {
  hle_gl_context* c = context_of(ctx);
  if (!c) {
    return kCGLBadContext;
  }
  hle_pixel_format* f = c->format;
  destroy_context(c);
  destroy_format(f);
  return kCGLNoError;
}

int CGLSetCurrentContext(void* ctx) {
  hle_gl_context* c = context_of(ctx);
  if (ctx && !c) {
    return kCGLBadContext;
  }
  make_current(c);
  return kCGLNoError;
}

void* CGLGetCurrentContext(void) {
  return current;
}

// ---------------------------------------------------------------------------
// GLU

// Whether |name| is one of the space-separated words of |extensions|.
Boolean gluCheckExtension(const uint8_t* name, const uint8_t* extensions) {
  if (!name || !extensions) {
    return 0;
  }
  size_t len = strlen((const char*)name);
  const char* p = (const char*)extensions;
  while ((p = strstr(p, (const char*)name))) {
    if ((p == (const char*)extensions || p[-1] == ' ') &&
        (p[len] == ' ' || p[len] == '\0')) {
      return 1;
    }
    p += len;
  }
  return 0;
}
