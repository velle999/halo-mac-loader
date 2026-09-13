// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// SDL: the real windows behind OpenGL drawing, and input.
//
// SDL video starts on first use, from the thread that first needs it,
// which is the main thread. Its events become Carbon events and the key
// and mouse state GetKeys and GetMouse report. While the game hides the
// cursor, the pointer is held in relative mode so aiming never runs into
// the edge of the screen.

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "gui.h"

int hle_cursor_hidden(void);
UInt32 GetCurrentKeyModifiers(void);

static int video_state;  // 0 untried, 1 running, -1 failed

// The SDL window the game draws in: a Carbon window's, or the one standing
// in for the screen when a context goes full screen.
static SDL_Window* game_window;
static hle_window* game_carbon_window;
static int game_fullscreen;
static Rect game_content;  // where the game believes its drawing is

static void pump(double max_wait);

int hle_sdl_video(void) {
  if (video_state == 0) {
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_WARP, "0");
    // SDL would turn SIGINT and SIGTERM into a quit event, which the game is
    // free to ignore; they should end the process as they do anywhere else.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO) == 0) {
      video_state = 1;
      hle_event_pump = pump;
    } else {
      fprintf(stderr, "hle: SDL video failed: %s\n", SDL_GetError());
      video_state = -1;
    }
  }
  return video_state > 0;
}

void* hle_sdl_window_for(hle_window* window, int fullscreen, int width,
                         int height) {
  if (!hle_sdl_video()) {
    return NULL;
  }
  // A window wholly off the main screen, as one made only to probe OpenGL
  // is, gets a hidden SDL window: a context draws there and nothing shows.
  hle_display_mode screen;
  hle_display_current(&screen);
  if (window && !fullscreen &&
      (window->content.right <= 0 || window->content.bottom <= 0 ||
       window->content.left >= screen.width ||
       window->content.top >= screen.height)) {
    if (!window->sdl_window) {
      window->sdl_window = SDL_CreateWindow(
          "", 0, 0, width > 0 ? width : 1, height > 0 ? height : 1,
          SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
      if (!window->sdl_window) {
        fprintf(stderr, "hle: hidden SDL window %dx%d: %s\n", width, height,
                SDL_GetError());
      }
    }
    return window->sdl_window;
  }
  if (window && window->sdl_window) {
    game_window = window->sdl_window;
  } else if (!window && game_window && !game_carbon_window) {
    // The stand-in for the screen already exists.
  } else {
    char* title = window && window->title ? cf_string_utf8(window->title)
                                          : NULL;
    SDL_Window* created = SDL_CreateWindow(
        title && *title ? title : "Halo", SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, width, height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI);
    free(title);
    if (!created) {
      fprintf(stderr, "hle: SDL_CreateWindow(%dx%d): %s\n", width, height,
              SDL_GetError());
      return NULL;
    }
    if (window) {
      window->sdl_window = created;
    }
    game_window = created;
  }
  game_carbon_window = window;

  if (fullscreen) {
    SDL_DisplayMode want = { 0, width, height, 0, NULL };
    SDL_DisplayMode closest;
    if (SDL_GetClosestDisplayMode(0, &want, &closest)) {
      SDL_SetWindowDisplayMode(game_window, &closest);
    }
    SDL_SetWindowFullscreen(game_window, SDL_WINDOW_FULLSCREEN);
    game_content.top = game_content.left = 0;
  } else {
    if (game_fullscreen) {
      SDL_SetWindowFullscreen(game_window, 0);
    }
    int w, h;
    SDL_GetWindowSize(game_window, &w, &h);
    if (w != width || h != height) {
      SDL_SetWindowSize(game_window, width, height);
    }
    game_content = window ? window->content : (Rect){ 0, 0, 0, 0 };
  }
  game_content.bottom = game_content.top + height;
  game_content.right = game_content.left + width;
  game_fullscreen = fullscreen;
  SDL_ShowWindow(game_window);
  SDL_RaiseWindow(game_window);
  return game_window;
}

void hle_sdl_window_destroy(hle_window* window) {
  if (!window || !window->sdl_window) {
    return;
  }
  if (window->sdl_window == game_window) {
    SDL_SetRelativeMouseMode(SDL_FALSE);
    game_window = NULL;
    game_carbon_window = NULL;
    game_fullscreen = 0;
  }
  SDL_DestroyWindow(window->sdl_window);
  window->sdl_window = NULL;
}

int hle_sdl_switch_mode(const hle_display_mode* mode) {
  if (!game_window || !game_fullscreen) {
    return 0;
  }
  SDL_DisplayMode want = { 0, mode->width, mode->height, mode->refresh, NULL };
  SDL_DisplayMode closest;
  if (!SDL_GetClosestDisplayMode(0, &want, &closest)) {
    return 0;
  }
  return SDL_SetWindowDisplayMode(game_window, &closest) == 0;
}

void hle_sdl_warp_mouse(int x, int y) {
  hle_input_mouse_position(x, y);
  if (game_window && !SDL_GetRelativeMouseMode()) {
    SDL_WarpMouseInWindow(game_window, x - game_content.left,
                          y - game_content.top);
  }
}

// ---------------------------------------------------------------------------
// Keys: SDL scancodes to Mac virtual key codes, US layout.

static const struct {
  SDL_Scancode scancode;
  UInt16 mac;
} kKeys[] = {
  { SDL_SCANCODE_A, 0x00 }, { SDL_SCANCODE_S, 0x01 }, { SDL_SCANCODE_D, 0x02 },
  { SDL_SCANCODE_F, 0x03 }, { SDL_SCANCODE_H, 0x04 }, { SDL_SCANCODE_G, 0x05 },
  { SDL_SCANCODE_Z, 0x06 }, { SDL_SCANCODE_X, 0x07 }, { SDL_SCANCODE_C, 0x08 },
  { SDL_SCANCODE_V, 0x09 }, { SDL_SCANCODE_B, 0x0B }, { SDL_SCANCODE_Q, 0x0C },
  { SDL_SCANCODE_W, 0x0D }, { SDL_SCANCODE_E, 0x0E }, { SDL_SCANCODE_R, 0x0F },
  { SDL_SCANCODE_Y, 0x10 }, { SDL_SCANCODE_T, 0x11 }, { SDL_SCANCODE_1, 0x12 },
  { SDL_SCANCODE_2, 0x13 }, { SDL_SCANCODE_3, 0x14 }, { SDL_SCANCODE_4, 0x15 },
  { SDL_SCANCODE_6, 0x16 }, { SDL_SCANCODE_5, 0x17 },
  { SDL_SCANCODE_EQUALS, 0x18 }, { SDL_SCANCODE_9, 0x19 },
  { SDL_SCANCODE_7, 0x1A }, { SDL_SCANCODE_MINUS, 0x1B },
  { SDL_SCANCODE_8, 0x1C }, { SDL_SCANCODE_0, 0x1D },
  { SDL_SCANCODE_RIGHTBRACKET, 0x1E }, { SDL_SCANCODE_O, 0x1F },
  { SDL_SCANCODE_U, 0x20 }, { SDL_SCANCODE_LEFTBRACKET, 0x21 },
  { SDL_SCANCODE_I, 0x22 }, { SDL_SCANCODE_P, 0x23 },
  { SDL_SCANCODE_RETURN, 0x24 }, { SDL_SCANCODE_L, 0x25 },
  { SDL_SCANCODE_J, 0x26 }, { SDL_SCANCODE_APOSTROPHE, 0x27 },
  { SDL_SCANCODE_K, 0x28 }, { SDL_SCANCODE_SEMICOLON, 0x29 },
  { SDL_SCANCODE_BACKSLASH, 0x2A }, { SDL_SCANCODE_COMMA, 0x2B },
  { SDL_SCANCODE_SLASH, 0x2C }, { SDL_SCANCODE_N, 0x2D },
  { SDL_SCANCODE_M, 0x2E }, { SDL_SCANCODE_PERIOD, 0x2F },
  { SDL_SCANCODE_TAB, 0x30 }, { SDL_SCANCODE_SPACE, 0x31 },
  { SDL_SCANCODE_GRAVE, 0x32 }, { SDL_SCANCODE_BACKSPACE, 0x33 },
  { SDL_SCANCODE_ESCAPE, 0x35 }, { SDL_SCANCODE_RGUI, 0x36 },
  { SDL_SCANCODE_LGUI, 0x37 }, { SDL_SCANCODE_LSHIFT, 0x38 },
  { SDL_SCANCODE_CAPSLOCK, 0x39 }, { SDL_SCANCODE_LALT, 0x3A },
  { SDL_SCANCODE_LCTRL, 0x3B }, { SDL_SCANCODE_RSHIFT, 0x3C },
  { SDL_SCANCODE_RALT, 0x3D }, { SDL_SCANCODE_RCTRL, 0x3E },
  { SDL_SCANCODE_KP_PERIOD, 0x41 }, { SDL_SCANCODE_KP_MULTIPLY, 0x43 },
  { SDL_SCANCODE_KP_PLUS, 0x45 }, { SDL_SCANCODE_NUMLOCKCLEAR, 0x47 },
  { SDL_SCANCODE_KP_DIVIDE, 0x4B }, { SDL_SCANCODE_KP_ENTER, 0x4C },
  { SDL_SCANCODE_KP_MINUS, 0x4E }, { SDL_SCANCODE_KP_EQUALS, 0x51 },
  { SDL_SCANCODE_KP_0, 0x52 }, { SDL_SCANCODE_KP_1, 0x53 },
  { SDL_SCANCODE_KP_2, 0x54 }, { SDL_SCANCODE_KP_3, 0x55 },
  { SDL_SCANCODE_KP_4, 0x56 }, { SDL_SCANCODE_KP_5, 0x57 },
  { SDL_SCANCODE_KP_6, 0x58 }, { SDL_SCANCODE_KP_7, 0x59 },
  { SDL_SCANCODE_KP_8, 0x5B }, { SDL_SCANCODE_KP_9, 0x5C },
  { SDL_SCANCODE_F5, 0x60 }, { SDL_SCANCODE_F6, 0x61 },
  { SDL_SCANCODE_F7, 0x62 }, { SDL_SCANCODE_F3, 0x63 },
  { SDL_SCANCODE_F8, 0x64 }, { SDL_SCANCODE_F9, 0x65 },
  { SDL_SCANCODE_F11, 0x67 }, { SDL_SCANCODE_F13, 0x69 },
  { SDL_SCANCODE_F14, 0x6B }, { SDL_SCANCODE_F10, 0x6D },
  { SDL_SCANCODE_F12, 0x6F }, { SDL_SCANCODE_F15, 0x71 },
  { SDL_SCANCODE_INSERT, 0x72 }, { SDL_SCANCODE_HOME, 0x73 },
  { SDL_SCANCODE_PAGEUP, 0x74 }, { SDL_SCANCODE_DELETE, 0x75 },
  { SDL_SCANCODE_F4, 0x76 }, { SDL_SCANCODE_END, 0x77 },
  { SDL_SCANCODE_F2, 0x78 }, { SDL_SCANCODE_PAGEDOWN, 0x79 },
  { SDL_SCANCODE_F1, 0x7A }, { SDL_SCANCODE_LEFT, 0x7B },
  { SDL_SCANCODE_RIGHT, 0x7C }, { SDL_SCANCODE_DOWN, 0x7D },
  { SDL_SCANCODE_UP, 0x7E },
};

static int mac_key(SDL_Scancode scancode) {
  for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); i++) {
    if (kKeys[i].scancode == scancode) {
      return kKeys[i].mac;
    }
  }
  return -1;
}

static int is_modifier(UInt16 mac) {
  return mac >= 0x36 && mac <= 0x3E;
}

// The character a key types, as the Mac's US layout would give it.
static UInt8 mac_char(const SDL_Keysym* key, int shifted) {
  switch (key->scancode) {
    case SDL_SCANCODE_RETURN: return 0x0D;
    case SDL_SCANCODE_KP_ENTER: return 0x03;
    case SDL_SCANCODE_TAB: return 0x09;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_ESCAPE: return 0x1B;
    case SDL_SCANCODE_DELETE: return 0x7F;
    case SDL_SCANCODE_LEFT: return 0x1C;
    case SDL_SCANCODE_RIGHT: return 0x1D;
    case SDL_SCANCODE_UP: return 0x1E;
    case SDL_SCANCODE_DOWN: return 0x1F;
    case SDL_SCANCODE_HOME: return 0x01;
    case SDL_SCANCODE_END: return 0x04;
    case SDL_SCANCODE_PAGEUP: return 0x0B;
    case SDL_SCANCODE_PAGEDOWN: return 0x0C;
    case SDL_SCANCODE_INSERT: return 0x05;
    default: break;
  }
  if (key->scancode >= SDL_SCANCODE_F1 && key->scancode <= SDL_SCANCODE_F12) {
    return 0x10;
  }
  SDL_Keycode sym = key->sym;
  if (sym <= 0 || sym >= 128) {
    return 0;
  }
  if (!shifted) {
    return sym;
  }
  if (sym >= 'a' && sym <= 'z') {
    return sym - 'a' + 'A';
  }
  static const char kPlain[] = "1234567890-=[]\\;',./`";
  static const char kShift[] = "!@#$%^&*()_+{}|:\"<>?~";
  const char* at = strchr(kPlain, sym);
  return at ? (UInt8)kShift[at - kPlain] : (UInt8)sym;
}

static void release_all_keys(void) {
  for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); i++) {
    hle_input_key(kKeys[i].mac, 0);
  }
  for (int b = 0; b < 3; b++) {
    hle_input_mouse_button(b, 0);
  }
}

static void post_key(const SDL_KeyboardEvent* e) {
  int code = mac_key(e->keysym.scancode);
  if (code < 0) {
    return;
  }
  int down = e->type == SDL_KEYDOWN;
  hle_input_key(code, down);
  UInt32 modifiers = GetCurrentKeyModifiers();
  UInt32 code32 = code;
  if (is_modifier(code)) {
    EventRef event = hle_event_new(kEventClassKeyboard,
                                   kEventRawKeyModifiersChanged);
    hle_event_set(event, kEventParamKeyModifiers, typeUInt32, 4, &modifiers);
    hle_post_event_owned(event);
    return;
  }
  UInt32 kind = !down ? kEventRawKeyUp
                      : e->repeat ? kEventRawKeyRepeat : kEventRawKeyDown;
  UInt8 ch = mac_char(&e->keysym, (e->keysym.mod & KMOD_SHIFT) != 0);
  EventRef event = hle_event_new(kEventClassKeyboard, kind);
  hle_event_set(event, kEventParamKeyCode, typeUInt32, 4, &code32);
  hle_event_set(event, kEventParamKeyMacCharCodes, typeChar, 1, &ch);
  hle_event_set(event, kEventParamKeyModifiers, typeUInt32, 4, &modifiers);
  hle_post_event_owned(event);
}

// ---------------------------------------------------------------------------
// The mouse

typedef struct {
  float x;
  float y;
} HIPoint;

static SInt16 mouse_x;
static SInt16 mouse_y;

static UInt16 mac_button(Uint8 sdl_button) {
  switch (sdl_button) {
    case SDL_BUTTON_LEFT: return 1;
    case SDL_BUTTON_RIGHT: return 2;
    case SDL_BUTTON_MIDDLE: return 3;
    default: return sdl_button;
  }
}

static EventRef mouse_event(UInt32 kind) {
  EventRef event = hle_event_new(kEventClassMouse, kind);
  HIPoint where = { mouse_x, mouse_y };
  UInt32 modifiers = GetCurrentKeyModifiers();
  hle_event_set(event, kEventParamMouseLocation, typeHIPoint, sizeof(where),
                &where);
  hle_event_set(event, kEventParamKeyModifiers, typeUInt32, 4, &modifiers);
  return event;
}

static void post_motion(const SDL_MouseMotionEvent* e) {
  if (SDL_GetRelativeMouseMode()) {
    // The pointer stays where the game last put it.
  } else {
    mouse_x = game_content.left + e->x;
    mouse_y = game_content.top + e->y;
    hle_input_mouse_position(mouse_x, mouse_y);
  }
  EventRef event = mouse_event(e->state ? kEventMouseDragged
                                        : kEventMouseMoved);
  Point delta = { .v = e->yrel, .h = e->xrel };
  hle_event_set(event, kEventParamMouseDelta, typeQDPoint, sizeof(delta),
                &delta);
  hle_post_event_owned(event);
}

static void post_button(const SDL_MouseButtonEvent* e) {
  int down = e->type == SDL_MOUSEBUTTONDOWN;
  UInt16 button = mac_button(e->button);
  hle_input_mouse_button(button - 1, down);
  EventRef event = mouse_event(down ? kEventMouseDown : kEventMouseUp);
  UInt32 clicks = e->clicks;
  hle_event_set(event, kEventParamMouseButton, typeMouseButton, 2, &button);
  hle_event_set(event, kEventParamClickCount, typeUInt32, 4, &clicks);
  hle_post_event_owned(event);
}

static void post_wheel(const SDL_MouseWheelEvent* e) {
  for (int axis = 0; axis < 2; axis++) {
    SInt32 delta = axis == 0 ? e->y : e->x;
    if (!delta) {
      continue;
    }
    // kEventMouseWheelAxisY is 1; X is 0.
    UInt16 which = axis == 0 ? 1 : 0;
    EventRef event = mouse_event(kEventMouseWheelMoved);
    hle_event_set(event, kEventParamMouseWheelAxis, typeMouseWheelAxis, 2,
                  &which);
    hle_event_set(event, kEventParamMouseWheelDelta, typeSInt32, 4, &delta);
    hle_post_event_owned(event);
  }
}

static void post_quit(void) {
  EventRef event = hle_event_new(kEventClassCommand, kEventCommandProcess);
  HICommandExtended command;
  memset(&command, 0, sizeof(command));
  command.commandID = kHICommandQuit;
  hle_event_set(event, kEventParamDirectObject, typeHICommand, sizeof(command),
                &command);
  hle_post_event_owned(event);
}

static void handle(const SDL_Event* e) {
  switch (e->type) {
    case SDL_QUIT:
      post_quit();
      break;
    case SDL_KEYDOWN:
    case SDL_KEYUP:
      post_key(&e->key);
      break;
    case SDL_MOUSEMOTION:
      post_motion(&e->motion);
      break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
      post_button(&e->button);
      break;
    case SDL_MOUSEWHEEL:
      post_wheel(&e->wheel);
      break;
    case SDL_WINDOWEVENT:
      if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
        release_all_keys();
        SDL_SetRelativeMouseMode(SDL_FALSE);
      }
      break;
  }
}

static void pump(double max_wait) {
  // Hold the pointer while the game hides it and has the window's focus.
  if (game_window) {
    int want = hle_cursor_hidden() &&
               (SDL_GetWindowFlags(game_window) & SDL_WINDOW_INPUT_FOCUS);
    if (want != (int)SDL_GetRelativeMouseMode()) {
      SDL_SetRelativeMouseMode(want ? SDL_TRUE : SDL_FALSE);
    }
  }
  SDL_Event e;
  int timeout_ms = max_wait < 0 ? -1 : (int)(max_wait * 1000);
  if (timeout_ms != 0 ? SDL_WaitEventTimeout(&e, timeout_ms < 0 ? 100
                                                               : timeout_ms)
                      : SDL_PollEvent(&e)) {
    handle(&e);
    while (SDL_PollEvent(&e)) {
      handle(&e);
    }
  }
}
