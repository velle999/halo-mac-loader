// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// The small parts of the toolbox the game touches before it has a window:
// keyboard and mouse state, window groups, the Script Manager, cursors, the
// Process Manager, the system UI mode and Apple Event handlers.

#define _GNU_SOURCE

#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "toolbox.h"

// ---------------------------------------------------------------------------
// Input state

// Carbon modifier bits.
enum {
  cmdKey = 0x0100,
  shiftKey = 0x0200,
  alphaLock = 0x0400,
  optionKey = 0x0800,
  controlKey = 0x1000,
};

static pthread_mutex_t input_lock = PTHREAD_MUTEX_INITIALIZER;
// A KeyMap as bytes: key k is bit (k & 7) of byte k >> 3, as on every Mac.
static uint8_t key_map[16];
static UInt32 button_state;
static SInt16 mouse_x;
static SInt16 mouse_y;

static int key_down(UInt16 code) {
  return code < 128 && (key_map[code >> 3] >> (code & 7)) & 1;
}

void hle_input_key(UInt16 code, int down) {
  if (code >= 128) {
    return;
  }
  pthread_mutex_lock(&input_lock);
  if (down) {
    key_map[code >> 3] |= 1 << (code & 7);
  } else {
    key_map[code >> 3] &= ~(1 << (code & 7));
  }
  pthread_mutex_unlock(&input_lock);
}

void hle_input_mouse_button(int button, int down) {
  pthread_mutex_lock(&input_lock);
  if (down) {
    button_state |= 1u << button;
  } else {
    button_state &= ~(1u << button);
  }
  pthread_mutex_unlock(&input_lock);
}

void hle_input_mouse_position(SInt16 x, SInt16 y) {
  mouse_x = x;
  mouse_y = y;
}

void GetKeys(uint8_t* keys) {
  pthread_mutex_lock(&input_lock);
  memcpy(keys, key_map, sizeof(key_map));
  pthread_mutex_unlock(&input_lock);
}

// The Mac virtual key codes of the modifier keys, left and right.
UInt32 GetCurrentKeyModifiers(void) {
  pthread_mutex_lock(&input_lock);
  UInt32 m = 0;
  if (key_down(0x37) || key_down(0x36)) {
    m |= cmdKey;
  }
  if (key_down(0x38) || key_down(0x3C)) {
    m |= shiftKey;
  }
  if (key_down(0x39)) {
    m |= alphaLock;
  }
  if (key_down(0x3A) || key_down(0x3D)) {
    m |= optionKey;
  }
  if (key_down(0x3B) || key_down(0x3E)) {
    m |= controlKey;
  }
  pthread_mutex_unlock(&input_lock);
  return m;
}

int Button(void) {
  return button_state & 1;
}

UInt32 GetCurrentEventButtonState(void) {
  return button_state;
}

void GetGlobalMouse(Point* where) {
  where->h = mouse_x;
  where->v = mouse_y;
}

// The game draws in one window at the origin, so local is global.
void GetMouse(Point* where) {
  GetGlobalMouse(where);
}

UInt32 GetDblTime(void) {
  return 30;
}

// US layout, by Mac virtual key code, unshifted then shifted.
static const char kUnshifted[128] =
    "asdfhgzxcv\0bqweryt123465=97-80]ou[ip\rlj'k;\\,/nm.\t `\b\0\x1b";
static const char kShifted[128] =
    "ASDFHGZXCV\0BQWERYT!@#$^%+(&_*)}OU{IP\rLJ\"K:|<?NM>\t ~\b\0\x1b";

UInt32 KeyTranslate(const void* table, UInt16 key_and_modifiers,
                    UInt32* state) {
  UInt16 code = key_and_modifiers & 0x7F;
  int shifted = (key_and_modifiers & (shiftKey | alphaLock)) != 0;
  return (unsigned char)(shifted ? kShifted : kUnshifted)[code];
}

// ---------------------------------------------------------------------------
// Window groups: kept for their level; windows do not layer here.

typedef struct {
  hle_target target;
  int32_t rc;
  UInt32 attributes;
  SInt32 level;
} window_group;

int CreateWindowGroup(UInt32 attributes, window_group** out) {
  window_group* group = calloc(1, sizeof(*group));
  hle_target_init(&group->target, hle_application_target());
  group->rc = 1;
  group->attributes = attributes;
  *out = group;
  return noErr;
}

int RetainWindowGroup(window_group* group) {
  group->rc++;
  return noErr;
}

int ReleaseWindowGroup(window_group* group) {
  if (group && --group->rc == 0) {
    hle_target_destroy(&group->target);
    free(group);
  }
  return noErr;
}

int SetWindowGroupLevel(window_group* group, SInt32 level) {
  group->level = level;
  return noErr;
}

int SetWindowGroup(void* window, window_group* group) {
  return noErr;
}

int IsWindowContainedInGroup(void* window, window_group* group) {
  return 0;
}

// ---------------------------------------------------------------------------
// The Script Manager: a Roman-script, US-English system.

long GetScriptVariable(SInt16 script, SInt16 selector) {
  return 0;
}

long GetScriptManagerVariable(SInt16 selector) {
  return 0;
}

SInt16 IntlScript(void) {
  return 0;
}

// ---------------------------------------------------------------------------
// Cursors. The SDL layer owns the real pointer.

static int cursor_hidden;

int hle_cursor_hidden(void) {
  return cursor_hidden > 0;
}

// Whether HideCursor has been called more often than ShowCursor. A game
// hides the pointer when it takes the mouse and shows it when it lets go;
// InitCursor, which shows the pointer at once, does not end that.
static int hide_requests;

int hle_cursor_hide_requested(void) {
  return hide_requests > 0;
}

void InitCursor(void) {
  cursor_hidden = 0;
}

void HideCursor(void) {
  cursor_hidden++;
  hide_requests++;
}

void ShowCursor(void) {
  if (cursor_hidden > 0) {
    cursor_hidden--;
  }
  if (hide_requests > 0) {
    hide_requests--;
  }
}

void SetCursor(const void* cursor) {
}

// A Cursor is 16x16 data and mask bits plus a hot spot: 68 bytes.
Handle GetCursor(SInt16 id) {
  static Handle blank;
  if (!blank) {
    blank = hle_handle_new(NULL, 68, 0);
  }
  return blank;
}

int QDRegisterNamedPixMapCursor(void* data, void* mask, Point hot_spot,
                                const char* name) {
  return noErr;
}

int QDSetNamedPixMapCursor(const char* name) {
  return noErr;
}

void SysBeep(SInt16 duration) {
}

// ---------------------------------------------------------------------------
// The Process Manager: this process is the only one, and it is in front.

typedef struct {
  UInt32 high;
  UInt32 low;
} ProcessSerialNumber;

#pragma pack(push, 2)
typedef struct {
  UInt32 processInfoLength;
  StringPtr processName;
  ProcessSerialNumber processNumber;
  UInt32 processType;
  OSType processSignature;
  UInt32 processMode;
  Ptr processLocation;
  UInt32 processSize;
  UInt32 processFreeMem;
  ProcessSerialNumber processLauncher;
  UInt32 processLaunchDate;
  UInt32 processActiveTime;
  FSSpec* processAppSpec;
} ProcessInfoRec;
#pragma pack(pop)

#ifdef __i386__
_Static_assert(sizeof(ProcessInfoRec) == 60, "ProcessInfoRec");
#endif

static const ProcessSerialNumber kThisProcess = { 0, 0x1000 };
// kCurrentProcess
static const ProcessSerialNumber kCurrentProcess = { 0, 2 };

static int is_this_process(const ProcessSerialNumber* psn) {
  return (psn->high == kThisProcess.high && psn->low == kThisProcess.low) ||
         (psn->high == kCurrentProcess.high &&
          psn->low == kCurrentProcess.low);
}

int GetCurrentProcess(ProcessSerialNumber* psn) {
  *psn = kThisProcess;
  return noErr;
}

int GetFrontProcess(ProcessSerialNumber* psn) {
  *psn = kThisProcess;
  return noErr;
}

int SetFrontProcess(const ProcessSerialNumber* psn) {
  return noErr;
}

int SameProcess(const ProcessSerialNumber* a, const ProcessSerialNumber* b,
                Boolean* result) {
  *result = is_this_process(a) == is_this_process(b) &&
            (is_this_process(a) ||
             (a->high == b->high && a->low == b->low));
  return noErr;
}

int GetProcessInformation(const ProcessSerialNumber* psn,
                          ProcessInfoRec* info) {
  if (!is_this_process(psn)) {
    return paramErr;
  }
  if (info->processName) {
    hle_name_to_pascal("Halo", info->processName, 32);
  }
  info->processNumber = kThisProcess;
  info->processType = 'APPL';
  info->processSignature = 'HALO';
  info->processMode = 0;
  info->processLocation = NULL;
  info->processSize = 0;
  info->processFreeMem = 0;
  info->processLauncher = kThisProcess;
  info->processLaunchDate = 0;
  info->processActiveTime = TickCount();
  if (info->processAppSpec) {
    char bundle[PATH_MAX];
    snprintf(bundle, sizeof(bundle), "%s", __darwin_executable_path);
    char* marker = strstr(bundle, "/Contents/MacOS/");
    if (marker) {
      *marker = '\0';
    }
    hle_path_to_spec(bundle, info->processAppSpec);
  }
  return noErr;
}

void ExitToShell(void) {
  fprintf(stderr, "hle: the game called ExitToShell from %p\n",
          __builtin_return_address(0));
  exit(0);
}

// ---------------------------------------------------------------------------
// System UI mode, Apple Events, hot keys

static UInt32 ui_mode;
static UInt32 ui_options;

int SetSystemUIMode(UInt32 mode, UInt32 options) {
  ui_mode = mode;
  ui_options = options;
  return noErr;
}

void GetSystemUIMode(UInt32* mode, UInt32* options) {
  if (mode) {
    *mode = ui_mode;
  }
  if (options) {
    *options = ui_options;
  }
}

// Nothing sends Apple Events here, so handlers are accepted and never run.
int AEInstallEventHandler(UInt32 event_class, UInt32 event_id, void* handler,
                          int32_t refcon, unsigned int system_handler) {
  return noErr;
}

int AEProcessAppleEvent(const EventRecord* event) {
  return noErr;
}

void* NewAEEventHandlerUPP(void* proc) {
  return proc;
}

typedef struct {
  OSType signature;
  UInt32 id;
} EventHotKeyID;

int RegisterEventHotKey(UInt32 key_code, UInt32 modifiers, EventHotKeyID id,
                        EventTargetRef target, UInt32 options, void** out) {
  static int registered;
  if (out) {
    *out = &registered;
  }
  return noErr;
}
