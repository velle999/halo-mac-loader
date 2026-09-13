// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Sound Manager channels, paced but silent for now.
//
// Each channel has a thread working through its command queue. A buffer
// holds the channel for as long as it would take to play, and callBackCmd
// runs the channel's callback after what came before it, so a game that
// feeds its next buffer from the callback keeps its rhythm.

#define _GNU_SOURCE

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "carbon.h"

enum {
  quietCmd = 3,
  flushCmd = 4,
  waitCmd = 10,
  pauseCmd = 11,
  resumeCmd = 12,
  callBackCmd = 13,
  availableCmd = 24,
  versionCmd = 25,
  ampCmd = 43,
  getAmpCmd = 45,
  volumeCmd = 46,
  getVolumeCmd = 47,
  bufferCmd = 81,
};

enum {
  queueFull = -203,
  badChannel = -205,
};

enum {
  kStdQLength = 128,
  kQueueCapacity = 256,
};

#pragma pack(push, 2)

typedef struct {
  UInt16 cmd;
  SInt16 param1;
  SInt32 param2;
} SndCommand;

typedef struct SndChannel {
  struct SndChannel* nextChan;
  Ptr firstMod;
  void (*callBack)(struct SndChannel* chan, SndCommand* cmd);
  SInt32 userInfo;
  SInt32 wait;
  SndCommand cmdInProgress;
  SInt16 flags;
  SInt16 qLength;
  SInt16 qHead;
  SInt16 qTail;
  SndCommand queue[kStdQLength];
} SndChannel;

typedef struct {
  UInt32 scStartTime;
  UInt32 scEndTime;
  UInt32 scCurrentTime;
  Boolean scChannelBusy;
  Boolean scChannelDisposed;
  Boolean scChannelPaused;
  Boolean scUnused;
  UInt32 scChannelAttributes;
  SInt32 scCPULoad;
} SCStatus;

// The fields the standard, extended and compressed sound headers share,
// then the extended ones. A standard header's second field is its length in
// frames; the others count channels there and frames at numFrames.
typedef struct {
  Ptr samplePtr;
  UInt32 lengthOrChannels;
  UInt32 sampleRate;  // 16.16
  UInt32 loopStart;
  UInt32 loopEnd;
  UInt8 encode;
  UInt8 baseFrequency;
  UInt32 numFrames;
  uint8_t aiffSampleRate[10];
  Ptr markerChunk;
  OSType format;  // compressed headers only
} SoundHeaderPrefix;

#pragma pack(pop)

#ifdef __i386__
_Static_assert(sizeof(SndChannel) == 1060, "SndChannel");
_Static_assert(offsetof(SoundHeaderPrefix, numFrames) == 22, "numFrames");
_Static_assert(offsetof(SoundHeaderPrefix, format) == 40, "format");
#endif

enum {
  stdSH = 0x00,
  extSH = 0xFF,
  cmpSH = 0xFE,
};

typedef struct channel {
  SndChannel* chan;
  int owned;
  pthread_t thread;
  pthread_mutex_t lock;
  pthread_cond_t cond;
  SndCommand queue[kQueueCapacity];
  int head;
  int count;
  int quit;
  int busy;
  int paused;
  int interrupt;
  UInt32 volume;
  SInt16 amplitude;
  struct channel* next;
} channel;

static pthread_mutex_t channels_lock = PTHREAD_MUTEX_INITIALIZER;
static channel* channels;

static channel* find_channel(SndChannel* chan) {
  pthread_mutex_lock(&channels_lock);
  channel* c = channels;
  while (c && c->chan != chan) {
    c = c->next;
  }
  pthread_mutex_unlock(&channels_lock);
  return c;
}

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
}

// How long a buffer would play, in nanoseconds.
static uint64_t buffer_duration(const SoundHeaderPrefix* header) {
  if (!header || header->sampleRate < 0x10000) {
    return 0;
  }
  double rate = header->sampleRate / 65536.0;
  double frames;
  if (header->encode == stdSH) {
    frames = header->lengthOrChannels;
  } else {
    frames = header->numFrames;
    if (header->encode == cmpSH) {
      // numFrames counts packets for the codecs that pack samples.
      switch (header->format) {
        case 'ima4': frames *= 64; break;
        case 'MAC3': frames *= 3; break;
        case 'MAC6': frames *= 6; break;
      }
    }
  }
  return (uint64_t)(frames / rate * 1e9);
}

// Sleeps until |until| unless the channel is told to stop. Called with the
// channel locked.
static void hold(channel* c, uint64_t until) {
  while (!c->quit && !c->interrupt) {
    uint64_t now = now_ns();
    if (now >= until) {
      break;
    }
    struct timespec ts = { until / 1000000000u, until % 1000000000u };
    pthread_cond_timedwait(&c->cond, &c->lock, &ts);
  }
}

static void* channel_main(void* arg) {
  channel* c = arg;
  pthread_mutex_lock(&c->lock);
  while (!c->quit) {
    if (c->count == 0 || c->paused) {
      pthread_cond_wait(&c->cond, &c->lock);
      continue;
    }
    SndCommand cmd = c->queue[c->head];
    c->head = (c->head + 1) % kQueueCapacity;
    c->count--;
    c->busy = 1;
    c->interrupt = 0;
    c->chan->cmdInProgress = cmd;
    pthread_cond_broadcast(&c->cond);
    switch (cmd.cmd) {
      case bufferCmd:
        hold(c, now_ns() + buffer_duration(
                               (const SoundHeaderPrefix*)(intptr_t)cmd.param2));
        break;
      case waitCmd:
        // param1 counts half milliseconds.
        hold(c, now_ns() + (uint64_t)(UInt16)cmd.param1 * 500000u);
        break;
      case callBackCmd:
        if (c->chan->callBack) {
          pthread_mutex_unlock(&c->lock);
          c->chan->callBack(c->chan, &cmd);
          pthread_mutex_lock(&c->lock);
        }
        break;
    }
    c->busy = 0;
    memset(&c->chan->cmdInProgress, 0, sizeof(SndCommand));
    pthread_cond_broadcast(&c->cond);
  }
  pthread_mutex_unlock(&c->lock);
  return NULL;
}

void* NewSndCallBackUPP(void* proc) {
  return proc;
}

void DisposeSndCallBackUPP(void* upp) {
}

int SndNewChannel(SndChannel** out, SInt16 synth, SInt32 init,
                  void (*callback)(SndChannel*, SndCommand*)) {
  if (!out) {
    return paramErr;
  }
  channel* c = calloc(1, sizeof(*c));
  c->chan = *out;
  if (!c->chan) {
    c->chan = calloc(1, sizeof(SndChannel));
    c->owned = 1;
  }
  c->chan->callBack = callback;
  c->chan->qLength = kStdQLength;
  c->volume = 0x01000100;
  c->amplitude = 255;
  pthread_mutex_init(&c->lock, NULL);
  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  pthread_cond_init(&c->cond, &attr);
  pthread_condattr_destroy(&attr);
  if (pthread_create(&c->thread, NULL, channel_main, c)) {
    if (c->owned) {
      free(c->chan);
    }
    free(c);
    return memFullErr;
  }
  pthread_mutex_lock(&channels_lock);
  c->next = channels;
  channels = c;
  pthread_mutex_unlock(&channels_lock);
  *out = c->chan;
  return noErr;
}

int SndDisposeChannel(SndChannel* chan, Boolean quiet_now) {
  channel* c = find_channel(chan);
  if (!c) {
    return badChannel;
  }
  pthread_mutex_lock(&c->lock);
  if (quiet_now) {
    c->count = 0;
    c->interrupt = 1;
  } else {
    while (c->count > 0 || c->busy) {
      if (pthread_equal(pthread_self(), c->thread)) {
        break;
      }
      pthread_cond_wait(&c->cond, &c->lock);
    }
  }
  c->quit = 1;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->lock);

  pthread_mutex_lock(&channels_lock);
  channel** link = &channels;
  while (*link != c) {
    link = &(*link)->next;
  }
  *link = c->next;
  pthread_mutex_unlock(&channels_lock);

  // A callback disposing of its own channel cannot wait for itself.
  if (pthread_equal(pthread_self(), c->thread)) {
    pthread_detach(c->thread);
    return noErr;
  }
  pthread_join(c->thread, NULL);
  if (c->owned) {
    free(c->chan);
  }
  free(c);
  return noErr;
}

int SndDoCommand(SndChannel* chan, const SndCommand* cmd, Boolean no_wait) {
  channel* c = find_channel(chan);
  if (!c || !cmd) {
    return badChannel;
  }
  pthread_mutex_lock(&c->lock);
  while (c->count == kQueueCapacity && !no_wait && !c->quit) {
    pthread_cond_wait(&c->cond, &c->lock);
  }
  int err = noErr;
  if (c->count == kQueueCapacity) {
    err = queueFull;
  } else {
    c->queue[(c->head + c->count) % kQueueCapacity] = *cmd;
    c->count++;
    pthread_cond_broadcast(&c->cond);
  }
  pthread_mutex_unlock(&c->lock);
  return err;
}

int SndDoImmediate(SndChannel* chan, const SndCommand* cmd) {
  channel* c = find_channel(chan);
  if (!c || !cmd) {
    return badChannel;
  }
  pthread_mutex_lock(&c->lock);
  switch (cmd->cmd) {
    case quietCmd:
      c->interrupt = 1;
      break;
    case flushCmd:
      c->count = 0;
      break;
    case pauseCmd:
      c->paused = 1;
      break;
    case resumeCmd:
      c->paused = 0;
      break;
    case volumeCmd:
      c->volume = cmd->param2;
      break;
    case getVolumeCmd:
      if (cmd->param2) {
        *(UInt32*)(intptr_t)cmd->param2 = c->volume;
      }
      break;
    case ampCmd:
      c->amplitude = cmd->param1;
      break;
    case getAmpCmd:
      if (cmd->param2) {
        *(SInt16*)(intptr_t)cmd->param2 = c->amplitude;
      }
      break;
    case availableCmd:
      if (cmd->param2) {
        *(SInt32*)(intptr_t)cmd->param2 = 1;
      }
      break;
    case versionCmd:
      if (cmd->param2) {
        *(UInt32*)(intptr_t)cmd->param2 = 0x00030000;
      }
      break;
    case callBackCmd:
      if (c->chan->callBack) {
        SndCommand copy = *cmd;
        pthread_mutex_unlock(&c->lock);
        c->chan->callBack(c->chan, &copy);
        return noErr;
      }
      break;
  }
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->lock);
  return noErr;
}

int SndChannelStatus(SndChannel* chan, SInt16 length, SCStatus* status) {
  channel* c = find_channel(chan);
  if (!c) {
    return badChannel;
  }
  if (!status || length < (SInt16)sizeof(SCStatus)) {
    return paramErr;
  }
  memset(status, 0, sizeof(*status));
  pthread_mutex_lock(&c->lock);
  status->scChannelBusy = c->busy || c->count > 0;
  status->scChannelPaused = c->paused;
  pthread_mutex_unlock(&c->lock);
  return noErr;
}

// Plays a 'snd ' resource. Nothing is audible yet, so there is nothing to
// wait for either.
int SndPlay(SndChannel* chan, Handle sound, Boolean async) {
  return sound ? noErr : paramErr;
}
