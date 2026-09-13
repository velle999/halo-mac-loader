// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// The Carbon Event Manager: events and their parameters, handlers on
// targets, the event queue, timers, and the loops that run them.
//
// Handlers, timers and the loops run on the thread that runs the loop;
// events may be posted from any thread. Handler and timer procedures are
// the game's code, so they are called as plain C function pointers: on
// Mac OS X a UPP is the procedure itself.

#define _GNU_SOURCE

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "toolbox.h"

#define EVENT_MAGIC 0x45766e74u
#define HANDLER_MAGIC 0x48646c72u
#define TIMER_MAGIC 0x546d7272u

void (*hle_event_pump)(double max_wait);
void (*hle_loop_hook)(void);

static double now_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

EventTime GetCurrentEventTime(void) {
  return now_seconds();
}

// ---------------------------------------------------------------------------
// Events

typedef struct {
  UInt32 name;
  UInt32 type;
  UInt32 size;
  uint8_t* data;
} param;

struct hle_event {
  uint32_t magic;
  int32_t rc;
  UInt32 event_class;
  UInt32 kind;
  EventTime when;
  UInt32 attributes;
  param* params;
  int param_count;
};

EventRef hle_event_new(UInt32 event_class, UInt32 kind) {
  struct hle_event* e = calloc(1, sizeof(*e));
  e->magic = EVENT_MAGIC;
  e->rc = 1;
  e->event_class = event_class;
  e->kind = kind;
  e->when = now_seconds();
  return e;
}

void hle_event_set(EventRef e, UInt32 name, UInt32 type, UInt32 size,
                   const void* data) {
  for (int i = 0; i < e->param_count; i++) {
    if (e->params[i].name == name) {
      free(e->params[i].data);
      e->params[i].type = type;
      e->params[i].size = size;
      e->params[i].data = malloc(size ? size : 1);
      memcpy(e->params[i].data, data, size);
      return;
    }
  }
  e->params = realloc(e->params, sizeof(param) * (e->param_count + 1));
  param* p = &e->params[e->param_count++];
  p->name = name;
  p->type = type;
  p->size = size;
  p->data = malloc(size ? size : 1);
  memcpy(p->data, data, size);
}

int CreateEvent(CFAllocatorRef allocator, UInt32 event_class, UInt32 kind,
                EventTime when, UInt32 attributes, EventRef* out) {
  EventRef e = hle_event_new(event_class, kind);
  if (when > 0) {
    e->when = when;
  }
  e->attributes = attributes;
  *out = e;
  return noErr;
}

EventRef RetainEvent(EventRef e) {
  if (e) {
    __sync_add_and_fetch(&e->rc, 1);
  }
  return e;
}

void ReleaseEvent(EventRef e) {
  if (!e || __sync_sub_and_fetch(&e->rc, 1) > 0) {
    return;
  }
  for (int i = 0; i < e->param_count; i++) {
    free(e->params[i].data);
  }
  free(e->params);
  e->magic = 0;
  free(e);
}

UInt32 GetEventClass(EventRef e) {
  return e->event_class;
}

UInt32 GetEventKind(EventRef e) {
  return e->kind;
}

int SetEventParameter(EventRef e, UInt32 name, UInt32 type, UInt32 size,
                      const void* data) {
  hle_event_set(e, name, type, size, data);
  return noErr;
}

// Point is { v, h } in shorts; HIPoint is { x, y } in floats.
static int coerce(const param* p, UInt32 desired, uint8_t* out,
                  UInt32* out_size) {
  if (p->type == typeQDPoint && desired == typeHIPoint && p->size >= 4) {
    SInt16 v;
    SInt16 h;
    memcpy(&v, p->data, 2);
    memcpy(&h, p->data + 2, 2);
    float xy[2] = { h, v };
    memcpy(out, xy, 8);
    *out_size = 8;
    return 1;
  }
  if (p->type == typeHIPoint && desired == typeQDPoint && p->size >= 8) {
    float xy[2];
    memcpy(xy, p->data, 8);
    SInt16 vh[2] = { (SInt16)lrintf(xy[1]), (SInt16)lrintf(xy[0]) };
    memcpy(out, vh, 4);
    *out_size = 4;
    return 1;
  }
  int int_types = (p->type == typeUInt32 || p->type == typeSInt32) &&
                  (desired == typeUInt32 || desired == typeSInt32);
  if (int_types && p->size == 4) {
    memcpy(out, p->data, 4);
    *out_size = 4;
    return 1;
  }
  return 0;
}

int GetEventParameter(EventRef e, UInt32 name, UInt32 desired,
                      UInt32* actual_type, UInt32 buffer_size,
                      UInt32* actual_size, void* data) {
  const param* p = NULL;
  for (int i = 0; i < e->param_count; i++) {
    if (e->params[i].name == name) {
      p = &e->params[i];
      break;
    }
  }
  if (!p) {
    return eventParameterNotFoundErr;
  }
  const uint8_t* bytes = p->data;
  UInt32 size = p->size;
  UInt32 type = p->type;
  uint8_t coerced[16];
  if (desired != typeWildCard && desired != p->type) {
    if (!coerce(p, desired, coerced, &size)) {
      return errAECoercionFail;
    }
    bytes = coerced;
    type = desired;
  }
  if (actual_type) {
    *actual_type = type;
  }
  if (actual_size) {
    *actual_size = size;
  }
  if (data) {
    memcpy(data, bytes, size < buffer_size ? size : buffer_size);
  }
  return noErr;
}

// ---------------------------------------------------------------------------
// Targets and handlers

typedef int (*EventHandlerProcPtr)(void* call_ref, EventRef event,
                                   void* user_data);

struct hle_handler {
  uint32_t magic;
  hle_target* target;
  EventHandlerProcPtr proc;
  void* user_data;
  EventTypeSpec* types;
  UInt32 type_count;
  int removed;
  struct hle_handler* next;
};

typedef struct {
  hle_target* target;
  struct hle_handler* next;
} call_ref;

static pthread_mutex_t handler_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static int dispatch_depth;
static struct hle_handler* removed_handlers;

static hle_target application_target;
// Sending to the dispatcher routes by class; it has no handlers of its own.
static hle_target dispatcher_target;
static hle_target* focus_target;

void hle_target_init(hle_target* target, hle_target* parent) {
  target->handlers = NULL;
  target->parent = parent;
}

hle_target* hle_application_target(void) {
  return &application_target;
}

void hle_set_focus(hle_target* target) {
  focus_target = target;
}

EventTargetRef GetApplicationEventTarget(void) {
  return &application_target;
}

EventTargetRef GetEventDispatcherTarget(void) {
  return &dispatcher_target;
}

// Every toolbox object starts with its target.
EventTargetRef GetWindowEventTarget(void* window) {
  return window;
}

EventTargetRef GetControlEventTarget(void* control) {
  return control;
}

EventTargetRef GetMenuEventTarget(void* menu) {
  return menu;
}

EventTargetRef GetUserFocusEventTarget(void) {
  return focus_target ? focus_target : &application_target;
}

static void unlink_handler(struct hle_handler* h) {
  struct hle_handler** link = &h->target->handlers;
  while (*link && *link != h) {
    link = &(*link)->next;
  }
  if (*link) {
    *link = h->next;
  }
  if (dispatch_depth) {
    // A dispatch may still be walking past it; free it once none is.
    h->removed = 1;
    h->next = removed_handlers;
    removed_handlers = h;
  } else {
    h->magic = 0;
    free(h->types);
    free(h);
  }
}

void hle_target_destroy(hle_target* target) {
  pthread_mutex_lock(&handler_lock);
  while (target->handlers) {
    unlink_handler(target->handlers);
  }
  pthread_mutex_unlock(&handler_lock);
}

int InstallEventHandler(EventTargetRef target, EventHandlerProcPtr proc,
                        UInt32 count, const EventTypeSpec* types,
                        void* user_data, EventHandlerRef* out) {
  if (!target || !proc) {
    return paramErr;
  }
  struct hle_handler* h = calloc(1, sizeof(*h));
  h->magic = HANDLER_MAGIC;
  h->target = target;
  h->proc = proc;
  h->user_data = user_data;
  h->type_count = count;
  h->types = malloc(sizeof(EventTypeSpec) * (count ? count : 1));
  memcpy(h->types, types, sizeof(EventTypeSpec) * count);
  pthread_mutex_lock(&handler_lock);
  h->next = target->handlers;
  target->handlers = h;
  pthread_mutex_unlock(&handler_lock);
  if (out) {
    *out = h;
  }
  return noErr;
}

int RemoveEventHandler(EventHandlerRef h) {
  if (!h || h->magic != HANDLER_MAGIC || h->removed) {
    return paramErr;
  }
  pthread_mutex_lock(&handler_lock);
  unlink_handler(h);
  pthread_mutex_unlock(&handler_lock);
  return noErr;
}

int AddEventTypesToHandler(EventHandlerRef h, UInt32 count,
                           const EventTypeSpec* types) {
  pthread_mutex_lock(&handler_lock);
  h->types = realloc(h->types, sizeof(EventTypeSpec) * (h->type_count + count));
  memcpy(h->types + h->type_count, types, sizeof(EventTypeSpec) * count);
  h->type_count += count;
  pthread_mutex_unlock(&handler_lock);
  return noErr;
}

int RemoveEventTypesFromHandler(EventHandlerRef h, UInt32 count,
                                const EventTypeSpec* types) {
  pthread_mutex_lock(&handler_lock);
  for (UInt32 i = 0; i < count; i++) {
    for (UInt32 k = 0; k < h->type_count; k++) {
      if (h->types[k].eventClass == types[i].eventClass &&
          h->types[k].eventKind == types[i].eventKind) {
        h->types[k] = h->types[--h->type_count];
        break;
      }
    }
  }
  pthread_mutex_unlock(&handler_lock);
  return noErr;
}

static int wants(const struct hle_handler* h, EventRef e) {
  for (UInt32 i = 0; i < h->type_count; i++) {
    if (h->types[i].eventClass == e->event_class &&
        h->types[i].eventKind == e->kind) {
      return 1;
    }
  }
  return 0;
}

// Runs handlers from |start| on |target|, then up the parents. A NULL
// |start| with |skip_target| set begins at the parent.
static int dispatch(hle_target* target, struct hle_handler* start,
                    int skip_target, EventRef e) {
  int status = eventNotHandledErr;
  pthread_mutex_lock(&handler_lock);
  dispatch_depth++;
  hle_target* t = skip_target ? target->parent : target;
  struct hle_handler* h = skip_target ? (t ? t->handlers : NULL)
                          : start ? start : t->handlers;
  while (t) {
    for (; h; h = h->next) {
      if (h->removed || !wants(h, e)) {
        continue;
      }
      call_ref ref = { t, h->next };
      status = h->proc(&ref, e, h->user_data);
      if (status != eventNotHandledErr) {
        goto done;
      }
    }
    t = t->parent;
    h = t ? t->handlers : NULL;
  }
done:
  if (--dispatch_depth == 0) {
    while (removed_handlers) {
      struct hle_handler* next = removed_handlers->next;
      removed_handlers->magic = 0;
      free(removed_handlers->types);
      free(removed_handlers);
      removed_handlers = next;
    }
  }
  pthread_mutex_unlock(&handler_lock);
  return status;
}

int CallNextEventHandler(void* ref, EventRef e) {
  call_ref* r = ref;
  return r->next ? dispatch(r->target, r->next, 0, e)
                 : dispatch(r->target, NULL, 1, e);
}

void QuitApplicationEventLoop(void);

static hle_target* route(EventRef e) {
  if (e->event_class == kEventClassKeyboard ||
      e->event_class == kEventClassMouse ||
      e->event_class == kEventClassCommand) {
    void* modal = hle_modal_window();
    if (modal) {
      return modal;
    }
    return focus_target ? focus_target : &application_target;
  }
  return &application_target;
}

int SendEventToEventTarget(EventRef e, EventTargetRef target) {
  if (!e || !target) {
    return paramErr;
  }
  if (target == &dispatcher_target) {
    target = route(e);
  }
  int status = dispatch(target, NULL, 0, e);
  // The standard application handler: a Quit nobody took ends the loop.
  if (status == eventNotHandledErr && e->event_class == kEventClassCommand &&
      e->kind == kEventCommandProcess) {
    HICommandExtended command;
    if (GetEventParameter(e, kEventParamDirectObject, typeHICommand, NULL,
                          sizeof(command), NULL, &command) == noErr &&
        command.commandID == kHICommandQuit) {
      QuitApplicationEventLoop();
      status = noErr;
    }
  }
  return status;
}

void* NewEventHandlerUPP(void* proc) {
  return proc;
}

void DisposeEventHandlerUPP(void* upp) {
}

// ---------------------------------------------------------------------------
// The queue

#define QUEUE_SIZE 512

static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static EventRef queue[QUEUE_SIZE];
static int queue_head;
static int queue_count;

void hle_post_event(EventRef e) {
  pthread_mutex_lock(&queue_lock);
  if (queue_count < QUEUE_SIZE) {
    queue[(queue_head + queue_count++) % QUEUE_SIZE] = RetainEvent(e);
  } else {
    cf_warn_once("the event queue overflowed");
  }
  pthread_mutex_unlock(&queue_lock);
}

void hle_post_event_owned(EventRef e) {
  hle_post_event(e);
  ReleaseEvent(e);
}

static int matches(EventRef e, UInt32 count, const EventTypeSpec* types) {
  if (count == 0) {
    return 1;
  }
  for (UInt32 i = 0; i < count; i++) {
    if (types[i].eventClass == e->event_class &&
        types[i].eventKind == e->kind) {
      return 1;
    }
  }
  return 0;
}

// The first queued event matching |types|; removed from the queue, and
// owned by the caller, when |pull| is set.
static EventRef find_queued(UInt32 count, const EventTypeSpec* types,
                            int pull) {
  EventRef found = NULL;
  pthread_mutex_lock(&queue_lock);
  for (int i = 0; i < queue_count; i++) {
    int at = (queue_head + i) % QUEUE_SIZE;
    if (!matches(queue[at], count, types)) {
      continue;
    }
    found = queue[at];
    if (pull) {
      for (int k = i; k > 0; k--) {
        queue[(queue_head + k) % QUEUE_SIZE] =
            queue[(queue_head + k - 1) % QUEUE_SIZE];
      }
      queue_head = (queue_head + 1) % QUEUE_SIZE;
      queue_count--;
    }
    break;
  }
  pthread_mutex_unlock(&queue_lock);
  return found;
}

void FlushEvents(UInt16 which, UInt16 stop) {
  pthread_mutex_lock(&queue_lock);
  while (queue_count) {
    EventRef e = queue[queue_head];
    queue_head = (queue_head + 1) % QUEUE_SIZE;
    queue_count--;
    ReleaseEvent(e);
  }
  pthread_mutex_unlock(&queue_lock);
}

// ---------------------------------------------------------------------------
// Timers

typedef void (*EventLoopTimerProcPtr)(void* timer, void* user_data);

typedef struct hle_timer {
  uint32_t magic;
  EventLoopTimerProcPtr proc;
  void* user_data;
  double fire_at;
  double interval;
  int removed;
  struct hle_timer* next;
} hle_timer;

static pthread_mutex_t timer_lock = PTHREAD_MUTEX_INITIALIZER;
static hle_timer* timers;

static struct {
  int unused;
} main_loop;

void* GetCurrentEventLoop(void) {
  return &main_loop;
}

void* GetMainEventLoop(void) {
  return &main_loop;
}

int InstallEventLoopTimer(void* loop, double delay, double interval,
                          EventLoopTimerProcPtr proc, void* user_data,
                          hle_timer** out) {
  hle_timer* t = calloc(1, sizeof(*t));
  t->magic = TIMER_MAGIC;
  t->proc = proc;
  t->user_data = user_data;
  t->fire_at = delay < 0 ? HUGE_VAL : now_seconds() + delay;
  t->interval = interval;
  pthread_mutex_lock(&timer_lock);
  t->next = timers;
  timers = t;
  pthread_mutex_unlock(&timer_lock);
  if (out) {
    *out = t;
  }
  return noErr;
}

int RemoveEventLoopTimer(hle_timer* t) {
  if (!t || t->magic != TIMER_MAGIC) {
    return paramErr;
  }
  t->removed = 1;
  return noErr;
}

void* NewEventLoopTimerUPP(void* proc) {
  return proc;
}

void DisposeEventLoopTimerUPP(void* upp) {
}

// Fires due timers; returns seconds until the next one.
static double run_timers(void) {
  double now = now_seconds();
  double next = HUGE_VAL;
  pthread_mutex_lock(&timer_lock);
  hle_timer** link = &timers;
  while (*link) {
    hle_timer* t = *link;
    if (t->removed) {
      *link = t->next;
      t->magic = 0;
      free(t);
      continue;
    }
    if (t->fire_at <= now) {
      if (t->interval > 0) {
        t->fire_at = now + t->interval;
      } else {
        t->fire_at = HUGE_VAL;
        t->removed = 1;
      }
      pthread_mutex_unlock(&timer_lock);
      t->proc(t, t->user_data);
      pthread_mutex_lock(&timer_lock);
      link = &timers;  // the procedure may have changed the list
      continue;
    }
    if (t->fire_at - now < next) {
      next = t->fire_at - now;
    }
    link = &t->next;
  }
  pthread_mutex_unlock(&timer_lock);
  return next;
}

// ---------------------------------------------------------------------------
// Loops

// Timers, then input for up to |max_wait|, which queues events.
static void pump_once(double max_wait) {
  if (hle_loop_hook) {
    hle_loop_hook();
  }
  double until_timer = run_timers();
  double wait = until_timer < max_wait ? until_timer : max_wait;
  if (wait < 0) {
    wait = 0;
  }
  if (hle_event_pump) {
    hle_event_pump(wait);
  } else if (wait > 0) {
    usleep((useconds_t)((wait < 0.02 ? wait : 0.02) * 1e6));
  }
}

static void run_once(double max_wait) {
  pump_once(max_wait);
  EventRef e;
  while ((e = find_queued(0, NULL, 1))) {
    SendEventToEventTarget(e, &dispatcher_target);
    ReleaseEvent(e);
  }
}

static int application_loops;
static int application_quit;

void RunApplicationEventLoop(void) {
  application_loops++;
  int depth = application_loops;
  while (!(application_quit >= depth)) {
    run_once(0.02);
  }
  application_quit = depth - 1;
  application_loops--;
}

void QuitApplicationEventLoop(void) {
  cf_trace("QuitApplicationEventLoop from %p", __builtin_return_address(0));
  if (application_loops > application_quit) {
    application_quit = application_loops;
  }
}

typedef struct modal_loop {
  void* window;
  int quit;
  struct modal_loop* outer;
} modal_loop;

static modal_loop* modal_loops;

void* hle_modal_window(void) {
  return modal_loops ? modal_loops->window : NULL;
}

int RunAppModalLoopForWindow(void* window) {
  modal_loop loop = { window, 0, modal_loops };
  modal_loops = &loop;
  while (!loop.quit) {
    run_once(0.02);
  }
  modal_loops = loop.outer;
  return noErr;
}

int QuitAppModalLoopForWindow(void* window) {
  for (modal_loop* l = modal_loops; l; l = l->outer) {
    if (l->window == window) {
      l->quit = 1;
      return noErr;
    }
  }
  return paramErr;
}

int ReceiveNextEvent(UInt32 count, const EventTypeSpec* types, double timeout,
                     unsigned int pull, EventRef* out) {
  double deadline = timeout < 0 ? HUGE_VAL : now_seconds() + timeout;
  for (;;) {
    if (hle_loop_hook) {
      hle_loop_hook();
    }
    double until_timer = run_timers();
    EventRef e = find_queued(count, types, pull & 0xff);
    if (e) {
      *out = e;
      return noErr;
    }
    if (application_loops && application_quit >= application_loops) {
      return eventLoopQuitErr;
    }
    double remaining = deadline - now_seconds();
    if (remaining <= 0) {
      return eventLoopTimedOutErr;
    }
    double wait = remaining < until_timer ? remaining : until_timer;
    if (hle_event_pump) {
      hle_event_pump(wait);
    } else {
      usleep((useconds_t)((wait < 0.01 ? wait : 0.01) * 1e6));
    }
  }
}

// ---------------------------------------------------------------------------
// Classic events

enum {
  mDownMask = 1 << mouseDown,
  mUpMask = 1 << mouseUp,
  keyDownMask = 1 << keyDown,
  keyUpMask = 1 << keyUp,
  autoKeyMask = 1 << autoKey,
};

void GetGlobalMouse(Point* where);
UInt32 GetCurrentKeyModifiers(void);

// A raw key or mouse button event as an EventRecord, or 0.
static int to_record(EventRef e, EventRecord* record) {
  UInt16 what = 0;
  if (e->event_class == kEventClassKeyboard) {
    what = e->kind == kEventRawKeyDown ? keyDown
           : e->kind == kEventRawKeyUp ? keyUp
           : e->kind == kEventRawKeyRepeat ? autoKey : 0;
  } else if (e->event_class == kEventClassMouse) {
    what = e->kind == kEventMouseDown ? mouseDown
           : e->kind == kEventMouseUp ? mouseUp : 0;
  }
  if (!what) {
    return 0;
  }
  memset(record, 0, sizeof(*record));
  record->what = what;
  record->when = (UInt32)(e->when * 60);
  if (what >= keyDown) {
    UInt32 code = 0;
    char ch = 0;
    GetEventParameter(e, kEventParamKeyCode, typeUInt32, NULL, 4, NULL, &code);
    GetEventParameter(e, kEventParamKeyMacCharCodes, typeChar, NULL, 1, NULL,
                      &ch);
    record->message = (code << 8) | (unsigned char)ch;
  }
  GetGlobalMouse(&record->where);
  record->modifiers = (UInt16)GetCurrentKeyModifiers();
  return 1;
}

int WaitNextEvent(UInt16 mask, EventRecord* record, UInt32 sleep_ticks,
                  void* mouse_region) {
  pump_once(sleep_ticks / 60.0 < 0.05 ? sleep_ticks / 60.0 : 0.05);
  // Key and mouse button events come back as EventRecords; the rest still
  // reach their handlers, as WaitNextEvent dispatches them on Mac OS X.
  EventRef e;
  while ((e = find_queued(0, NULL, 1))) {
    EventRecord converted;
    if (to_record(e, &converted) && (mask & (1 << converted.what))) {
      *record = converted;
      ReleaseEvent(e);
      return 1;
    }
    SendEventToEventTarget(e, &dispatcher_target);
    ReleaseEvent(e);
  }
  memset(record, 0, sizeof(*record));
  record->when = TickCount();
  GetGlobalMouse(&record->where);
  record->modifiers = (UInt16)GetCurrentKeyModifiers();
  return 0;
}

int ConvertEventRefToEventRecord(EventRef e, EventRecord* record) {
  return to_record(e, record);
}

int IsShowContextualMenuEvent(EventRef e) {
  return 0;
}
