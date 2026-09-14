// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// A sampling profiler for the game, for machines with no profiler
// installed.
//
// HLE_PROFILE=<file> starts it at the game's first frame. For each
// millisecond of CPU time the process uses, SIGPROF records the address the
// thread that used it was at, the game's code that thread was called from,
// and the thread. At exit the counts are written to <file> with the
// process's mappings, for tools/profile_report.py.
//
// The kernel signals the thread that was running when the millisecond ran
// out, unless that thread blocks SIGPROF, as SDL's threads do; the sound
// callback calls hle_profile_thread so that its thread is sampled too.

#define _GNU_SOURCE

#include "profile.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

#if defined(__i386__)

enum {
  kSlots = 1 << 18,   // distinct (address, caller, thread) triples kept
  kProbes = 32,
  kStackChunk = 256,  // stack words read at a time, looking for a caller
  kStackChunks = 8,
};

typedef struct {
  uint32_t address;
  uint32_t caller;
  uint32_t thread;
  uint32_t count;
} sample_slot;

static sample_slot* slots;
static int table_busy;
static uint32_t slots_used;
static uint32_t samples;
static uint32_t dropped;
static uintptr_t game_lo;
static uintptr_t game_hi;
static pid_t pid;
static char* output;

static int in_game(uintptr_t address) {
  return address >= game_lo && address < game_hi;
}

// Whether |address| in the game's code follows a call instruction, so that
// a word on a stack holding it is likely a return address.
static int after_call(uintptr_t address) {
  if (address < game_lo + 7 || address >= game_hi) {
    return 0;
  }
  const uint8_t* p = (const uint8_t*)address;
  if (p[-5] == 0xe8) {
    return 1;  // call rel32
  }
  // call *r/m32: 0xff, then a ModRM byte whose middle bits are 010.
  return (p[-2] == 0xff &&
          ((p[-1] & 0xf8) == 0xd0 ||  // *%reg
           ((p[-1] & 0xf8) == 0x10 && (p[-1] & 7) != 4 &&
            (p[-1] & 7) != 5))) ||  // *(%reg)
         (p[-3] == 0xff &&
          (((p[-2] & 0xf8) == 0x50 && (p[-2] & 7) != 4) ||  // *disp8(%reg)
           p[-2] == 0x14)) ||                                // *(sib)
         (p[-4] == 0xff && p[-3] == 0x54) ||                 // *disp8(sib)
         (p[-6] == 0xff &&
          (((p[-5] & 0xf8) == 0x90 && (p[-5] & 7) != 4) ||  // *disp32(%reg)
           p[-5] == 0x15)) ||                                // *address
         (p[-7] == 0xff && p[-6] == 0x94);                   // *disp32(sib)
}

// The game's code a thread at |address| was called from: in the game, the
// return address in the current frame, as the game keeps frame pointers;
// elsewhere, the first return address into the game up the stack.
static uint32_t find_caller(uintptr_t address, uintptr_t esp, uintptr_t ebp) {
  uintptr_t words[kStackChunk];
  if (in_game(address)) {
    struct iovec local = { words, 2 * sizeof(uintptr_t) };
    struct iovec remote = { (void*)ebp, 2 * sizeof(uintptr_t) };
    if (process_vm_readv(pid, &local, 1, &remote, 1, 0) ==
            (ssize_t)(2 * sizeof(uintptr_t)) &&
        after_call(words[1])) {
      return words[1];
    }
    return 0;
  }
  for (int chunk = 0; chunk < kStackChunks; chunk++) {
    struct iovec local = { words, sizeof(words) };
    struct iovec remote = { (void*)(esp + chunk * sizeof(words)),
                            sizeof(words) };
    ssize_t got = process_vm_readv(pid, &local, 1, &remote, 1, 0);
    for (ssize_t i = 0; (i + 1) * (ssize_t)sizeof(uintptr_t) <= got; i++) {
      if (after_call(words[i])) {
        return words[i];
      }
    }
    if (got < (ssize_t)sizeof(words)) {
      break;
    }
  }
  return 0;
}

static void record(uint32_t address, uint32_t caller, uint32_t thread) {
  if (__sync_lock_test_and_set(&table_busy, 1)) {
    __sync_fetch_and_add(&dropped, 1);
    return;
  }
  uint32_t hash = (address * 2654435761u) ^ (caller * 2246822519u) ^ thread;
  for (int i = 0; i < kProbes; i++) {
    sample_slot* s = &slots[(hash + i) & (kSlots - 1)];
    if (s->count &&
        (s->address != address || s->caller != caller ||
         s->thread != thread)) {
      continue;
    }
    if (!s->count) {
      if (slots_used >= kSlots / 4 * 3) {
        break;
      }
      s->address = address;
      s->caller = caller;
      s->thread = thread;
      slots_used++;
    }
    s->count++;
    samples++;
    __sync_lock_release(&table_busy);
    return;
  }
  __sync_fetch_and_add(&dropped, 1);
  __sync_lock_release(&table_busy);
}

static void on_sample(int signum, siginfo_t* info, void* context) {
  int saved_errno = errno;
  greg_t* r = ((ucontext_t*)context)->uc_mcontext.gregs;
  uintptr_t address = (uintptr_t)r[REG_EIP];
  record(address,
         find_caller(address, (uintptr_t)r[REG_ESP], (uintptr_t)r[REG_EBP]),
         (uint32_t)syscall(SYS_gettid));
  errno = saved_errno;
}

static void write_profile(void) {
  struct itimerval off;
  memset(&off, 0, sizeof(off));
  setitimer(ITIMER_PROF, &off, NULL);
  // Nothing is recorded after this.
  while (__sync_lock_test_and_set(&table_busy, 1)) {
    sched_yield();
  }
  FILE* f = fopen(output, "w");
  if (!f) {
    fprintf(stderr, "hle: HLE_PROFILE: cannot write %s: %m\n", output);
    return;
  }
  fprintf(f, "# hle profile: %u samples, %u dropped, each a millisecond of "
          "CPU time\n", samples, dropped);
  fprintf(f, "# game code %#lx-%#lx, main thread %d\n",
          (unsigned long)game_lo, (unsigned long)game_hi, (int)pid);
  FILE* maps = fopen("/proc/self/maps", "r");
  if (maps) {
    char line[512];
    while (fgets(line, sizeof(line), maps)) {
      fprintf(f, "# map %s", line);
    }
    fclose(maps);
  }
  fprintf(f, "# count thread address caller symbol+offset\n");
  for (uint32_t i = 0; i < kSlots; i++) {
    const sample_slot* s = &slots[i];
    if (!s->count) {
      continue;
    }
    Dl_info info;
    const char* symbol = "?";
    unsigned long offset = 0;
    if (!in_game(s->address) && dladdr((void*)(uintptr_t)s->address, &info) &&
        info.dli_sname) {
      symbol = info.dli_sname;
      offset = s->address - (uintptr_t)info.dli_saddr;
    }
    fprintf(f, "%u %u %#x %#x %s+%#lx\n", s->count, s->thread, s->address,
            s->caller, symbol, offset);
  }
  fclose(f);
  fprintf(stderr, "hle: a profile of %u samples is in %s\n", samples, output);
}

void hle_profile_start(void* game_code) {
  static int started;
  const char* path = getenv("HLE_PROFILE");
  if (started || !path || !*path) {
    return;
  }
  started = 1;
  FILE* maps = fopen("/proc/self/maps", "r");
  if (maps) {
    char line[512];
    while (fgets(line, sizeof(line), maps)) {
      unsigned long lo, hi;
      if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 &&
          (uintptr_t)game_code >= lo && (uintptr_t)game_code < hi) {
        game_lo = lo;
        game_hi = hi;
      }
    }
    fclose(maps);
  }
  sample_slot* table = mmap(NULL, kSlots * sizeof(*table),
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (!game_hi || table == MAP_FAILED) {
    fprintf(stderr, "hle: HLE_PROFILE: the game's code was not found\n");
    return;
  }
  pid = getpid();
  output = strdup(path);
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_sigaction = on_sample;
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&action.sa_mask);
  sigaction(SIGPROF, &action, NULL);
  slots = table;
  atexit(write_profile);
  struct itimerval every = { { 0, 1000 }, { 0, 1000 } };
  setitimer(ITIMER_PROF, &every, NULL);
  fprintf(stderr, "hle: profiling the game's code at %#lx-%#lx into %s\n",
          (unsigned long)game_lo, (unsigned long)game_hi, output);
}

void hle_profile_thread(void) {
  static __thread int unblocked;
  if (!slots || unblocked) {
    return;
  }
  unblocked = 1;
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGPROF);
  pthread_sigmask(SIG_UNBLOCK, &set, NULL);
}

#else

void hle_profile_start(void* game_code) {
  if (getenv("HLE_PROFILE")) {
    fprintf(stderr, "hle: HLE_PROFILE samples i386 code only\n");
  }
}

void hle_profile_thread(void) {
}

#endif
