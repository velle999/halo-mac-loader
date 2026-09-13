// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// The HIToolbox side of Carbon: events, windows, controls and menus.
//
// Every toolbox object this library hands out -- the application, windows,
// controls, menus -- starts with an hle_target, so an EventTargetRef is the
// object's address. The SDL layer feeds input through hle_event_pump; the
// window code answers dialogs through hle_loop_hook.

#ifndef HLE_TOOLBOX_H_
#define HLE_TOOLBOX_H_

#include "carbon.h"

typedef double EventTime;
typedef struct hle_event* EventRef;
typedef struct hle_target* EventTargetRef;
typedef struct hle_handler* EventHandlerRef;

typedef struct {
  UInt32 eventClass;
  UInt32 eventKind;
} EventTypeSpec;

#pragma pack(push, 2)

typedef struct {
  UInt16 what;
  UInt32 message;
  UInt32 when;
  Point where;
  UInt16 modifiers;
} EventRecord;

typedef struct {
  UInt32 attributes;
  UInt32 commandID;
  union {
    void* control;
    void* window;
    struct {
      void* menuRef;
      UInt16 menuItemIndex;
    } menu;
  } source;
} HICommandExtended;

#pragma pack(pop)

_Static_assert(sizeof(EventRecord) == 16, "EventRecord");
#ifdef __i386__
_Static_assert(sizeof(HICommandExtended) == 14, "HICommandExtended");
#endif

enum {
  kEventClassMouse = 'mous',
  kEventClassKeyboard = 'keyb',
  kEventClassApplication = 'appl',
  kEventClassMenu = 'menu',
  kEventClassWindow = 'wind',
  kEventClassControl = 'cntl',
  kEventClassCommand = 'cmds',
};

enum {
  kEventMouseDown = 1,
  kEventMouseUp = 2,
  kEventMouseMoved = 5,
  kEventMouseDragged = 6,
  kEventMouseWheelMoved = 10,
  kEventRawKeyDown = 1,
  kEventRawKeyRepeat = 2,
  kEventRawKeyUp = 3,
  kEventRawKeyModifiersChanged = 4,
  kEventCommandProcess = 1,
};

enum {
  kEventParamDirectObject = '----',
  kEventParamMouseLocation = 'mloc',
  kEventParamMouseDelta = 'mdta',
  kEventParamMouseButton = 'mbtn',
  kEventParamClickCount = 'ccnt',
  kEventParamMouseWheelAxis = 'mwax',
  kEventParamMouseWheelDelta = 'mwdl',
  kEventParamKeyCode = 'kcod',
  kEventParamKeyMacCharCodes = 'kchr',
  kEventParamKeyModifiers = 'kmod',
};

enum {
  typeWildCard = '****',
  typeHICommand = 'hcmd',
  typeQDPoint = 'QDpt',
  typeHIPoint = 'hipt',
  typeUInt32 = 'magn',
  typeSInt32 = 'long',
  typeSInt16 = 'shor',
  typeUInt16 = 'ushr',
  typeBoolean = 'bool',
  typeChar = 'TEXT',
  typeMouseButton = 'mbtn',
  typeMouseWheelAxis = 'mwax',
  typeControlRef = 'ctrl',
  typeWindowRef = 'wind',
  typeMenuRef = 'menu',
};

enum {
  kHICommandOK = 'ok  ',
  kHICommandCancel = 'not!',
  kHICommandQuit = 'quit',
};

enum {
  eventParameterNotFoundErr = -9870,
  eventNotHandledErr = -9874,
  eventLoopTimedOutErr = -9875,
  eventLoopQuitErr = -9876,
  errAECoercionFail = -1700,
};

// Classic event kinds and masks.
enum {
  nullEvent = 0,
  mouseDown = 1,
  mouseUp = 2,
  keyDown = 3,
  keyUp = 4,
  autoKey = 5,
};

#define kEventDurationForever (-1.0)

// An object events can be sent to. Newer handlers run first; an event
// nobody handles moves on to the parent.
typedef struct hle_target {
  struct hle_handler* handlers;
  struct hle_target* parent;
} hle_target;

void hle_target_init(hle_target* target, hle_target* parent);
// Removes every handler on |target|, as disposing of its object does.
void hle_target_destroy(hle_target* target);
hle_target* hle_application_target(void);

// The target keyboard, mouse and command events go to: the window of the
// innermost modal loop, else whatever hle_set_focus named, else the
// application.
void hle_set_focus(hle_target* target);

EventRef hle_event_new(UInt32 event_class, UInt32 kind);
void hle_event_set(EventRef event, UInt32 name, UInt32 type, UInt32 size,
                   const void* data);
// Queues |event| for the event loop, which sends it to the dispatcher. The
// queue takes a reference of its own; the _owned form takes the caller's.
void hle_post_event(EventRef event);
void hle_post_event_owned(EventRef event);

// Waits up to |max_wait| seconds for input and posts what arrives. Set by
// the SDL layer; without it the loops only sleep.
extern void (*hle_event_pump)(double max_wait);
// Runs once per event-loop pass. The window code uses it for dialogs.
extern void (*hle_loop_hook)(void);

// The window of the innermost modal loop, or NULL.
void* hle_modal_window(void);

// Input state, kept for GetKeys, Button and GetMouse.
void hle_input_key(UInt16 mac_key_code, int down);
void hle_input_mouse_button(int button, int down);
void hle_input_mouse_position(SInt16 x, SInt16 y);

EventTime GetCurrentEventTime(void);
EventRef RetainEvent(EventRef event);
void ReleaseEvent(EventRef event);
int SendEventToEventTarget(EventRef event, EventTargetRef target);
UInt32 TickCount(void);

#endif  // HLE_TOOLBOX_H_
