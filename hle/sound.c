// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Sound Manager channels over one SDL audio device.
//
// Each channel has a thread working through its command queue, and a voice,
// the buffer it is playing, which SDL's audio callback mixes (sound_mix.c).
// A bufferCmd starts the voice, from the queue or at once through
// SndDoImmediate, and what is queued after it waits until the buffer has
// played, so the callBackCmd a game queues behind a buffer tells it that
// the sound has ended.
//
// Callbacks run one at a time, as they do on a Mac. Halo's returns without
// doing anything while another is running.
//
// HLE_SOUND=0, or no audio device, leaves channels silent: a buffer takes
// the time it would take to play and nothing is heard.

#define _GNU_SOURCE

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL2/SDL.h>

#include "carbon.h"
#include "sound_mix.h"

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
  rateMultiplierCmd = 86,
  getRateMultiplierCmd = 87,
};

enum {
  initNoInterp = 0x0004,
};

enum {
  queueFull = -203,
  badChannel = -205,
};

enum {
  kStdQLength = 128,
  kQueueCapacity = 256,
  kMaxMixedChannels = 64,
  kOutputRate = 44100,
  kOutputFrames = 1024,
  kTracedBuffers = 6,
};

// How often a channel looks at a buffer that is playing: when it should
// have ended, but never sooner or later than these.
static const uint64_t kMinVoiceWait = 2000000;
static const uint64_t kMaxVoiceWait = 100000000;

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

#pragma pack(pop)

#ifdef __i386__
_Static_assert(sizeof(SndChannel) == 1060, "SndChannel");
#endif

typedef struct channel {
  SndChannel* chan;
  int owned;
  int mixed;  // in the device's list
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
  // Changed with the channel's lock and the device's both held; the audio
  // callback reads and advances it holding the device's.
  hle_sound_voice voice;
  // For a buffer not mixed: when it would have finished playing.
  uint64_t silent_end;
  UInt32 volume;
  SInt16 amplitude;
  struct channel* next;
} channel;

static pthread_mutex_t channels_lock = PTHREAD_MUTEX_INITIALIZER;
static channel* channels;

static pthread_mutex_t callback_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

// The device is chosen by the first SndNewChannel and kept, so every channel
// sees the same one.
static pthread_mutex_t device_choice_lock = PTHREAD_MUTEX_INITIALIZER;
static int device_chosen;
static SDL_AudioDeviceID device;
static int device_rate;
static int32_t* mix_sums;
static int mix_capacity;
static channel* mixed[kMaxMixedChannels];
static int mixed_count;

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

static void lock_voices(void) {
  if (device) {
    SDL_LockAudioDevice(device);
  }
}

static void unlock_voices(void) {
  if (device) {
    SDL_UnlockAudioDevice(device);
  }
}

static void mix(void* userdata, Uint8* stream, int length) {
  int16_t* out = (int16_t*)stream;
  int frames = length / 4;
  for (int done = 0; done < frames;) {
    int n = frames - done < mix_capacity ? frames - done : mix_capacity;
    memset(mix_sums, 0, sizeof(int32_t) * 2 * n);
    for (int i = 0; i < mixed_count; i++) {
      if (!mixed[i]->paused) {
        hle_sound_voice_mix(&mixed[i]->voice, mix_sums, n, device_rate);
      }
    }
    for (int i = 0; i < 2 * n; i++) {
      int32_t sum = mix_sums[i];
      out[2 * done + i] = sum > 32767 ? 32767 : sum < -32768 ? -32768 : sum;
    }
    done += n;
  }
}

static void choose_device(void) {
  pthread_mutex_lock(&device_choice_lock);
  if (!device_chosen) {
    device_chosen = 1;
    const char* sound = getenv("HLE_SOUND");
    if (sound && strcmp(sound, "0") == 0) {
      cf_trace("sound: HLE_SOUND=0, so channels are silent");
    } else {
      // As in sdl.c: SIGINT and SIGTERM should end the process.
      SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
      SDL_AudioSpec want;
      SDL_AudioSpec have;
      memset(&want, 0, sizeof(want));
      want.freq = kOutputRate;
      want.format = AUDIO_S16SYS;
      want.channels = 2;
      want.samples = kOutputFrames;
      want.callback = mix;
      if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "hle: SDL audio failed: %s\n", SDL_GetError());
      } else if (!(device = SDL_OpenAudioDevice(
                       NULL, 0, &want, &have,
                       SDL_AUDIO_ALLOW_FREQUENCY_CHANGE))) {
        fprintf(stderr, "hle: no audio device: %s\n", SDL_GetError());
      } else {
        device_rate = have.freq;
        mix_capacity = have.samples ? have.samples : kOutputFrames;
        mix_sums = malloc(sizeof(int32_t) * 2 * mix_capacity);
        SDL_PauseAudioDevice(device, 0);
        cf_trace("sound: %s, %d Hz, %d frames a callback",
                 SDL_GetCurrentAudioDriver(), have.freq, have.samples);
      }
    }
  }
  pthread_mutex_unlock(&device_choice_lock);
}

static void unmix(channel* c) {
  lock_voices();
  for (int i = 0; i < mixed_count; i++) {
    if (mixed[i] == c) {
      mixed[i] = mixed[--mixed_count];
      break;
    }
  }
  c->mixed = 0;
  c->voice.playing = 0;
  unlock_voices();
}

// How jagged 16-bit samples are read in one byte order: the sum of the
// steps between neighbouring samples on the same side.
static uint64_t roughness(const hle_sound_buffer* buffer, int big_endian) {
  uint32_t samples = buffer->frames * buffer->channels;
  if (samples > 8192) {
    samples = 8192;
  }
  uint64_t sum = 0;
  for (uint32_t i = buffer->channels; i < samples; i++) {
    const uint8_t* p = buffer->data + 2 * i;
    const uint8_t* q = p - 2 * buffer->channels;
    int a = big_endian ? (int16_t)(p[0] << 8 | p[1])
                       : (int16_t)(p[1] << 8 | p[0]);
    int b = big_endian ? (int16_t)(q[0] << 8 | q[1])
                       : (int16_t)(q[1] << 8 | q[0]);
    sum += (uint64_t)abs(a - b);
  }
  return sum;
}

// Describes the first buffers a run plays, for HLE_TRACE.
static void trace_buffer(const uint8_t* header, const hle_sound_buffer* buffer,
                         int status, uint32_t format) {
  static int traced;
  if (!header || traced >= kTracedBuffers) {
    return;
  }
  traced++;
  char code[5] = "none";
  if (format) {
    for (int i = 0; i < 4; i++) {
      char ch = (char)(format >> (24 - 8 * i));
      code[i] = ch >= 0x20 && ch < 0x7f ? ch : '.';
    }
  }
  char order[96] = "";
  if (status == kHleSoundOk && buffer->bytes == 2 && buffer->frames > 1) {
    uint64_t big = roughness(buffer, 1);
    uint64_t little = roughness(buffer, 0);
    snprintf(order, sizeof(order), "; smoother read %s-endian (%llu, %llu)",
             big <= little ? "big" : "little", (unsigned long long)big,
             (unsigned long long)little);
  }
  cf_trace("sound: buffer, encode %#x: %u channels, %u-bit, %.0f Hz, %u "
           "frames, format %s%s%s", header[offsetof(SoundHeader, encode)],
           buffer->channels, buffer->bytes * 8, buffer->rate / 65536.0,
           buffer->frames, code, status == kHleSoundOk ? "" : ", not played",
           order);
}

// Starts |c| playing the buffer |header| describes, in place of anything
// it was playing. Called with the channel locked.
static void start_voice(channel* c, const uint8_t* header) {
  hle_sound_buffer buffer;
  uint32_t format = 0;
  int status = hle_sound_buffer_from_header(header, &buffer, &format);
  trace_buffer(header, &buffer, status, format);
  if (status == kHleSoundUnsupported) {
    cf_warn_once("sound: a compressed buffer, which is silent");
  }
  int audible = status == kHleSoundOk && c->mixed;
  lock_voices();
  c->voice.buffer = buffer;
  c->voice.position = 0;
  c->voice.playing = audible;
  uint32_t rate_multiplier = c->voice.rate_multiplier;
  unlock_voices();
  c->silent_end = 0;
  if (!audible && status != kHleSoundBadHeader) {
    uint64_t duration = hle_sound_header_duration_ns(header, rate_multiplier);
    if (duration) {
      c->silent_end = now_ns() + duration;
    }
  }
}

// Called with the channel locked.
static void stop_voice(channel* c) {
  lock_voices();
  c->voice.playing = 0;
  unlock_voices();
  c->silent_end = 0;
}

// Whether |c| is still playing a buffer. Called with the channel locked.
static int voice_busy(channel* c) {
  if (c->silent_end) {
    if (now_ns() < c->silent_end) {
      return 1;
    }
    c->silent_end = 0;
    return 0;
  }
  lock_voices();
  int playing = c->voice.playing;
  unlock_voices();
  return playing;
}

// Waits a while for the buffer |c| is playing to end, or for a command
// that changes it. Called with the channel locked.
static void wait_for_voice(channel* c) {
  uint64_t wait;
  if (c->silent_end) {
    uint64_t now = now_ns();
    wait = c->silent_end > now ? c->silent_end - now : 0;
  } else {
    lock_voices();
    wait = hle_sound_voice_remaining_ns(&c->voice);
    unlock_voices();
  }
  wait = wait < kMinVoiceWait ? kMinVoiceWait
         : wait > kMaxVoiceWait ? kMaxVoiceWait : wait;
  uint64_t until = now_ns() + wait;
  struct timespec ts = { until / 1000000000u, until % 1000000000u };
  pthread_cond_timedwait(&c->cond, &c->lock, &ts);
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

// Called with the channel unlocked.
static void run_callback(channel* c, SndCommand* cmd) {
  if (c->chan->callBack) {
    pthread_mutex_lock(&callback_lock);
    c->chan->callBack(c->chan, cmd);
    pthread_mutex_unlock(&callback_lock);
  }
}

static void* channel_main(void* arg) {
  channel* c = arg;
  pthread_mutex_lock(&c->lock);
  while (!c->quit) {
    if (voice_busy(c)) {
      wait_for_voice(c);
      continue;
    }
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
        start_voice(c, (const uint8_t*)(intptr_t)cmd.param2);
        break;
      case waitCmd:
        // param1 counts half milliseconds.
        hold(c, now_ns() + (uint64_t)(UInt16)cmd.param1 * 500000u);
        break;
      case callBackCmd:
        pthread_mutex_unlock(&c->lock);
        run_callback(c, &cmd);
        pthread_mutex_lock(&c->lock);
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
  choose_device();
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
  c->voice.rate_multiplier = 0x10000;
  c->voice.left = 0x0100;
  c->voice.right = 0x0100;
  c->voice.amplitude = 255;
  c->voice.interpolate = !(init & initNoInterp);
  pthread_mutex_init(&c->lock, NULL);
  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  pthread_cond_init(&c->cond, &attr);
  pthread_condattr_destroy(&attr);
  lock_voices();
  if (device && mixed_count < kMaxMixedChannels) {
    mixed[mixed_count++] = c;
    c->mixed = 1;
  }
  unlock_voices();
  if (pthread_create(&c->thread, NULL, channel_main, c)) {
    unmix(c);
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
  int own_thread = pthread_equal(pthread_self(), c->thread);
  pthread_mutex_lock(&c->lock);
  if (quiet_now) {
    c->count = 0;
    c->interrupt = 1;
    stop_voice(c);
  } else if (!own_thread) {
    while (c->count > 0 || c->busy || voice_busy(c)) {
      wait_for_voice(c);
    }
  }
  c->quit = 1;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->lock);
  unmix(c);

  pthread_mutex_lock(&channels_lock);
  channel** link = &channels;
  while (*link != c) {
    link = &(*link)->next;
  }
  *link = c->next;
  pthread_mutex_unlock(&channels_lock);

  // A callback disposing of its own channel cannot wait for itself.
  if (own_thread) {
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
    case bufferCmd:
      start_voice(c, (const uint8_t*)(intptr_t)cmd->param2);
      break;
    case quietCmd:
      stop_voice(c);
      c->interrupt = 1;
      break;
    case flushCmd:
      c->count = 0;
      break;
    case pauseCmd:
    case resumeCmd:
      lock_voices();
      c->paused = cmd->cmd == pauseCmd;
      unlock_voices();
      break;
    case volumeCmd:
      // The right side's volume in the high word, the left's in the low.
      c->volume = cmd->param2;
      lock_voices();
      c->voice.left = (UInt16)(c->volume & 0xFFFF);
      c->voice.right = (UInt16)(c->volume >> 16);
      unlock_voices();
      break;
    case getVolumeCmd:
      if (cmd->param2) {
        *(UInt32*)(intptr_t)cmd->param2 = c->volume;
      }
      break;
    case ampCmd:
      c->amplitude = cmd->param1;
      lock_voices();
      c->voice.amplitude =
          cmd->param1 < 0 ? 0 : cmd->param1 > 255 ? 255 : cmd->param1;
      unlock_voices();
      break;
    case getAmpCmd:
      if (cmd->param2) {
        *(SInt16*)(intptr_t)cmd->param2 = c->amplitude;
      }
      break;
    case rateMultiplierCmd:
      lock_voices();
      c->voice.rate_multiplier = (UInt32)cmd->param2;
      unlock_voices();
      break;
    case getRateMultiplierCmd:
      if (cmd->param2) {
        lock_voices();
        UInt32 rate_multiplier = c->voice.rate_multiplier;
        unlock_voices();
        *(UInt32*)(intptr_t)cmd->param2 = rate_multiplier;
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
    case callBackCmd: {
      SndCommand copy = *cmd;
      pthread_mutex_unlock(&c->lock);
      run_callback(c, &copy);
      return noErr;
    }
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
  status->scChannelBusy = voice_busy(c) || c->busy || c->count > 0;
  status->scChannelPaused = c->paused;
  pthread_mutex_unlock(&c->lock);
  return noErr;
}

// Plays a 'snd ' resource. The resource format is not read yet, so there
// is nothing to hear or wait for.
int SndPlay(SndChannel* chan, Handle sound, Boolean async) {
  return sound ? noErr : paramErr;
}
