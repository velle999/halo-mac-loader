// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Sound Manager channels, mixed on one audio clock.
//
// The mixer works through each channel's command queue itself, as the
// Sound Manager does: when a buffer ends, what was queued after it runs at
// that sample. A callBackCmd reports the end on time, and a buffer the
// callback queues starts where the last one stopped. Halo streams its music
// in 550-frame buffers that way; a queue worked anywhere else left a gap
// after each one.
//
// The clock is SDL's audio device. With HLE_SOUND=0, or no device, a thread
// runs the mixer in time and nothing is heard.
//
// All channel state is guarded by the clock's lock, which the mixer holds
// and which is recursive. Callbacks run in the mixer, one at a time, and may
// send commands to any channel. Halo's callback returns without doing
// anything while another is running.

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
  kOutputRate = 44100,
  kOutputFrames = 1024,
  // Commands one channel runs in one pass of the mixer at most, so a
  // callback that queues only another callback cannot hold the mixer.
  kCommandsPerPass = 64,
  kTracedBuffers = 6,
};

// SndDoCommand, waiting for room in a full queue, and SndDisposeChannel,
// waiting for a channel to finish, look this often and give up after this.
static const uint64_t kPollInterval = 5000000;
static const uint64_t kMaxWait = 10000000000u;

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
  SndCommand queue[kQueueCapacity];
  int head;
  int count;
  int paused;
  int callbacks;  // of this channel, running now
  int disposed;   // freed once none of its callbacks is running
  hle_sound_voice voice;
  // Frames to pass before the next command: a waitCmd's, or those of a
  // buffer that is not mixed but still takes its time.
  uint32_t silent_frames;
  UInt32 volume;
  SInt16 amplitude;
  struct channel* next;
} channel;

static channel* channels;

// The clock is chosen by the first SndNewChannel, before there is a channel
// to mix, and kept.
static pthread_mutex_t clock_choice_lock = PTHREAD_MUTEX_INITIALIZER;
static int clock_chosen;
static SDL_AudioDeviceID device;
static pthread_mutex_t silent_clock_lock =
    PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static int output_rate = kOutputRate;
static int32_t* sums;
static int sums_capacity;
static __thread int mixing;

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
}

static void sleep_ns(uint64_t ns) {
  struct timespec ts = { ns / 1000000000u, ns % 1000000000u };
  nanosleep(&ts, NULL);
}

static void lock_clock(void) {
  if (device) {
    SDL_LockAudioDevice(device);
  } else {
    pthread_mutex_lock(&silent_clock_lock);
  }
}

static void unlock_clock(void) {
  if (device) {
    SDL_UnlockAudioDevice(device);
  } else {
    pthread_mutex_unlock(&silent_clock_lock);
  }
}

// Called with the clock locked, as is everything below that reads or
// changes a channel.
static channel* find_channel(SndChannel* chan) {
  for (channel* c = channels; c; c = c->next) {
    if (c->chan == chan && !c->disposed) {
      return c;
    }
  }
  return NULL;
}

static int playing_or_queued(const channel* c) {
  return c->voice.playing || c->silent_frames || c->count > 0;
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

// Starts |c| on the buffer |header| describes, in place of what it was
// playing.
static void start_voice(channel* c, const uint8_t* header) {
  hle_sound_buffer buffer;
  uint32_t format = 0;
  int status = hle_sound_buffer_from_header(header, &buffer, &format);
  trace_buffer(header, &buffer, status, format);
  if (status == kHleSoundUnsupported) {
    cf_warn_once("sound: a compressed buffer, which is silent");
  }
  int audible = status == kHleSoundOk && device;
  c->voice.buffer = buffer;
  c->voice.position = 0;
  c->voice.playing = audible;
  c->silent_frames = 0;
  if (!audible && status != kHleSoundBadHeader) {
    uint64_t ns = hle_sound_header_duration_ns(header,
                                               c->voice.rate_multiplier);
    c->silent_frames = (uint32_t)(ns * (uint64_t)output_rate / 1000000000u);
  }
}

static void stop_voice(channel* c) {
  c->voice.playing = 0;
  c->silent_frames = 0;
}

static void run_callback(channel* c, const SndCommand* cmd) {
  if (!c->chan->callBack) {
    return;
  }
  SndCommand copy = *cmd;
  c->callbacks++;
  c->chan->callBack(c->chan, &copy);
  c->callbacks--;
}

// Carries out a command from |c|'s queue, in the mixer.
static void run_queued(channel* c, const SndCommand* cmd) {
  c->chan->cmdInProgress = *cmd;
  switch (cmd->cmd) {
    case bufferCmd:
      start_voice(c, (const uint8_t*)(intptr_t)cmd->param2);
      break;
    case waitCmd:
      // param1 counts half milliseconds.
      c->silent_frames =
          (uint32_t)((uint64_t)(UInt16)cmd->param1 * output_rate / 2000);
      break;
    case callBackCmd:
      run_callback(c, cmd);
      break;
  }
  // A callback may have disposed of the channel, and the game of its record.
  if (!c->disposed) {
    memset(&c->chan->cmdInProgress, 0, sizeof(SndCommand));
  }
}

// Mixes |c| into the first |frames| frames of the sums, working through its
// queue whenever what it plays ends.
static void mix_channel(channel* c, int frames) {
  int done = 0;
  int commands = 0;
  while (done < frames && !c->disposed) {
    if (c->voice.playing) {
      done += hle_sound_voice_mix(&c->voice, sums + 2 * done, frames - done,
                                  output_rate);
      if (c->voice.playing) {
        break;
      }
    } else if (c->silent_frames) {
      uint32_t n = (uint32_t)(frames - done);
      if (n > c->silent_frames) {
        n = c->silent_frames;
      }
      c->silent_frames -= n;
      done += n;
    } else if (c->count > 0 && !c->paused && commands++ < kCommandsPerPass) {
      SndCommand cmd = c->queue[c->head];
      c->head = (c->head + 1) % kQueueCapacity;
      c->count--;
      run_queued(c, &cmd);
    } else {
      break;
    }
  }
}

// Frees the disposed channels none of whose callbacks is running. Not
// during the mixer's pass, which may still hold one.
static void free_disposed(void) {
  channel** link = &channels;
  while (*link) {
    channel* c = *link;
    if (c->disposed && c->callbacks == 0) {
      *link = c->next;
      if (c->owned) {
        free(c->chan);
      }
      free(c);
    } else {
      link = &c->next;
    }
  }
}

// What the mixer did, traced every ten seconds. A callback that comes late
// means the device ran dry and the sound broke up; clipped samples are
// voices summing past full scale.
static struct {
  uint64_t window_start;
  uint64_t last_start;
  uint64_t longest_gap;
  uint64_t longest_mix;
  unsigned callbacks;
  unsigned late;
  unsigned clipped;
  int most_voices;
} stats;

static void count_mix(uint64_t start, int frames, unsigned clipped,
                      int voices) {
  uint64_t end = now_ns();
  uint64_t period = (uint64_t)frames * 1000000000u / output_rate;
  if (!stats.last_start) {
    stats.window_start = start;
  } else {
    uint64_t gap = start - stats.last_start;
    if (gap > stats.longest_gap) {
      stats.longest_gap = gap;
    }
    if (gap > period + period / 2) {
      stats.late++;
    }
  }
  stats.last_start = start;
  stats.callbacks++;
  if (end - start > stats.longest_mix) {
    stats.longest_mix = end - start;
  }
  stats.clipped += clipped;
  if (voices > stats.most_voices) {
    stats.most_voices = voices;
  }
  if (start - stats.window_start >= 10000000000u) {
    cf_trace("sound: %u callbacks in %.1f s, %u late (the longest gap %.1f "
             "ms), the longest mix %.2f ms, %u samples clipped, at most %d "
             "voices", stats.callbacks, (start - stats.window_start) / 1e9,
             stats.late, stats.longest_gap / 1e6, stats.longest_mix / 1e6,
             stats.clipped, stats.most_voices);
    memset(&stats, 0, sizeof(stats));
    stats.window_start = start;
    stats.last_start = start;
  }
}

static void mix(void* userdata, Uint8* stream, int length) {
  uint64_t start = now_ns();
  int16_t* out = (int16_t*)stream;
  int frames = length / 4;
  unsigned clipped = 0;
  int voices = 0;
  mixing = 1;
  for (int done = 0; done < frames;) {
    int n = frames - done < sums_capacity ? frames - done : sums_capacity;
    memset(sums, 0, sizeof(int32_t) * 2 * n);
    int playing = 0;
    for (channel* c = channels; c; c = c->next) {
      playing += c->voice.playing;
      mix_channel(c, n);
    }
    if (playing > voices) {
      voices = playing;
    }
    for (int i = 0; i < 2 * n; i++) {
      int32_t sum = sums[i];
      if (sum > 32767 || sum < -32768) {
        clipped++;
        sum = sum > 32767 ? 32767 : -32768;
      }
      out[2 * done + i] = sum;
    }
    done += n;
  }
  mixing = 0;
  free_disposed();
  count_mix(start, frames, clipped, voices);
}

// Without a device, runs the mixer in time into a buffer no one hears.
static void* silent_clock(void* arg) {
  int16_t* scratch = malloc(sizeof(int16_t) * 2 * kOutputFrames);
  uint64_t period = (uint64_t)kOutputFrames * 1000000000u / kOutputRate;
  uint64_t next = now_ns();
  for (;;) {
    pthread_mutex_lock(&silent_clock_lock);
    mix(NULL, (Uint8*)scratch, 4 * kOutputFrames);
    pthread_mutex_unlock(&silent_clock_lock);
    next += period;
    uint64_t now = now_ns();
    if (next > now) {
      sleep_ns(next - now);
    } else if (now - next > 8 * period) {
      next = now;
    }
  }
  return NULL;
}

static void open_device(void) {
  // As in sdl.c: SIGINT and SIGTERM should end the process.
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    fprintf(stderr, "hle: SDL audio failed: %s\n", SDL_GetError());
    return;
  }
  SDL_AudioSpec want;
  SDL_AudioSpec have;
  memset(&want, 0, sizeof(want));
  want.freq = kOutputRate;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = kOutputFrames;
  want.callback = mix;
  SDL_AudioDeviceID opened = SDL_OpenAudioDevice(
      NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
  if (!opened) {
    fprintf(stderr, "hle: no audio device: %s\n", SDL_GetError());
    return;
  }
  output_rate = have.freq;
  sums_capacity = have.samples ? have.samples : kOutputFrames;
  sums = malloc(sizeof(int32_t) * 2 * sums_capacity);
  device = opened;
  SDL_PauseAudioDevice(device, 0);
  cf_trace("sound: %s, %d Hz, %d frames a callback",
           SDL_GetCurrentAudioDriver(), have.freq, have.samples);
}

static void choose_clock(void) {
  pthread_mutex_lock(&clock_choice_lock);
  if (!clock_chosen) {
    clock_chosen = 1;
    const char* sound = getenv("HLE_SOUND");
    if (!sound || strcmp(sound, "0") != 0) {
      open_device();
    }
    if (!device) {
      sums_capacity = kOutputFrames;
      sums = malloc(sizeof(int32_t) * 2 * sums_capacity);
      pthread_t thread;
      if (pthread_create(&thread, NULL, silent_clock, NULL) == 0) {
        pthread_detach(thread);
      }
      cf_trace("sound: silent; channels keep time");
    }
  }
  pthread_mutex_unlock(&clock_choice_lock);
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
  choose_clock();
  channel* c = calloc(1, sizeof(*c));
  if (!c) {
    return memFullErr;
  }
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
  lock_clock();
  c->next = channels;
  channels = c;
  unlock_clock();
  *out = c->chan;
  return noErr;
}

int SndDisposeChannel(SndChannel* chan, Boolean quiet_now) {
  lock_clock();
  channel* c = find_channel(chan);
  if (!c) {
    unlock_clock();
    return badChannel;
  }
  // Otherwise what is queued and playing finishes first. The mixer needs
  // the lock to get there, and a callback, in the mixer, cannot wait.
  uint64_t give_up = now_ns() + kMaxWait;
  while (!quiet_now && !mixing && playing_or_queued(c) &&
         now_ns() < give_up) {
    unlock_clock();
    sleep_ns(kPollInterval);
    lock_clock();
    if (!(c = find_channel(chan))) {
      unlock_clock();
      return noErr;
    }
  }
  c->disposed = 1;
  c->count = 0;
  stop_voice(c);
  if (!mixing) {
    free_disposed();
  }
  unlock_clock();
  return noErr;
}

int SndDoCommand(SndChannel* chan, const SndCommand* cmd, Boolean no_wait) {
  lock_clock();
  channel* c = find_channel(chan);
  // A full queue empties only as the mixer plays, which needs the lock, and
  // a callback, in the mixer, cannot wait.
  uint64_t give_up = now_ns() + kMaxWait;
  while (c && cmd && c->count == kQueueCapacity && !no_wait && !mixing &&
         now_ns() < give_up) {
    unlock_clock();
    sleep_ns(kPollInterval);
    lock_clock();
    c = find_channel(chan);
  }
  int err = noErr;
  if (!c || !cmd) {
    err = badChannel;
  } else if (c->count == kQueueCapacity) {
    err = queueFull;
  } else {
    c->queue[(c->head + c->count) % kQueueCapacity] = *cmd;
    c->count++;
  }
  unlock_clock();
  return err;
}

int SndDoImmediate(SndChannel* chan, const SndCommand* cmd) {
  lock_clock();
  channel* c = find_channel(chan);
  if (!c || !cmd) {
    unlock_clock();
    return badChannel;
  }
  switch (cmd->cmd) {
    case bufferCmd:
      start_voice(c, (const uint8_t*)(intptr_t)cmd->param2);
      break;
    case quietCmd:
      stop_voice(c);
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
      // The right side's volume in the high word, the left's in the low.
      c->volume = cmd->param2;
      c->voice.left = (UInt16)(c->volume & 0xFFFF);
      c->voice.right = (UInt16)(c->volume >> 16);
      break;
    case getVolumeCmd:
      if (cmd->param2) {
        *(UInt32*)(intptr_t)cmd->param2 = c->volume;
      }
      break;
    case ampCmd:
      c->amplitude = cmd->param1;
      c->voice.amplitude =
          cmd->param1 < 0 ? 0 : cmd->param1 > 255 ? 255 : cmd->param1;
      break;
    case getAmpCmd:
      if (cmd->param2) {
        *(SInt16*)(intptr_t)cmd->param2 = c->amplitude;
      }
      break;
    case rateMultiplierCmd:
      c->voice.rate_multiplier = (UInt32)cmd->param2;
      break;
    case getRateMultiplierCmd:
      if (cmd->param2) {
        *(UInt32*)(intptr_t)cmd->param2 = c->voice.rate_multiplier;
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
      run_callback(c, cmd);
      break;
  }
  unlock_clock();
  return noErr;
}

int SndChannelStatus(SndChannel* chan, SInt16 length, SCStatus* status) {
  lock_clock();
  channel* c = find_channel(chan);
  int err = noErr;
  if (!c) {
    err = badChannel;
  } else if (!status || length < (SInt16)sizeof(SCStatus)) {
    err = paramErr;
  } else {
    memset(status, 0, sizeof(*status));
    status->scChannelBusy = playing_or_queued(c) || c->callbacks > 0;
    status->scChannelPaused = c->paused;
  }
  unlock_clock();
  return err;
}

// Plays a 'snd ' resource. The resource format is not read yet, so there
// is nothing to hear or wait for.
int SndPlay(SndChannel* chan, Handle sound, Boolean async) {
  return sound ? noErr : paramErr;
}
