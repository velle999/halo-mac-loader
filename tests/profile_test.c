// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Checks hle/profile.c: a child process, taking this program's code as the
// game's, spends CPU time in one of its functions and then in libc's
// memset, and its profile must put the samples there, each with the call
// that led to it.
//
//   make tests/profile_test && tests/profile_test

#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../hle/profile.h"

static int failures;

static void check(int ok, const char* what) {
  if (!ok) {
    printf("FAIL: %s\n", what);
    failures++;
  }
}

static double cpu_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

__attribute__((noinline)) static uint32_t spin(uint32_t n) {
  uint32_t x = 1;
  for (uint32_t i = 0; i < n; i++) {
    x = x * 1664525u + 1013904223u + i;
  }
  return x;
}

static char buffer[1 << 20];
static void* (*volatile fill)(void*, int, size_t) = memset;

// Half a second of CPU time in spin, then half a second in memset.
__attribute__((noinline)) static void work(void) {
  volatile uint32_t sink = 0;
  double until = cpu_seconds() + 0.5;
  while (cpu_seconds() < until) {
    sink += spin(1000000);
  }
  until = cpu_seconds() + 0.5;
  while (cpu_seconds() < until) {
    fill(buffer, (int)sink++, sizeof(buffer));
  }
}

// The function a return address's call instruction called, when the call
// is a direct one.
static uintptr_t called_by(uintptr_t return_address) {
  const uint8_t* p = (const uint8_t*)return_address;
  if (p[-5] != 0xe8) {
    return 0;
  }
  int32_t relative;
  memcpy(&relative, p - 4, sizeof(relative));
  return return_address + relative;
}

int main(void) {
  char path[] = "/tmp/profile_test.XXXXXX";
  int fd = mkstemp(path);
  if (fd < 0) {
    perror("mkstemp");
    return 1;
  }
  close(fd);

  pid_t child = fork();
  if (child == 0) {
    setenv("HLE_PROFILE", path, 1);
    hle_profile_start((void*)work);
    work();
    // As the crash report does: the profile is written now, and the child
    // leaves without running what exit would.
    hle_profile_write();
    _exit(0);
  }
  int status = 0;
  waitpid(child, &status, 0);
  check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "the child exits");

  FILE* f = fopen(path, "r");
  check(f != NULL, "the profile is written");
  unsigned long game_lo = 0, game_hi = 0;
  uint64_t total = 0, in_spin = 0, spin_called = 0, outside = 0,
           outside_called = 0;
  char line[1024];
  while (f && fgets(line, sizeof(line), f)) {
    if (sscanf(line, "# game code %lx-%lx", &game_lo, &game_hi) == 2) {
      continue;
    }
    unsigned count, thread;
    unsigned long address, caller;
    if (line[0] == '#' ||
        sscanf(line, "%u %u %lx %lx", &count, &thread, &address, &caller) !=
            4) {
      continue;
    }
    total += count;
    if (address >= (uintptr_t)spin && address < (uintptr_t)spin + 256) {
      in_spin += count;
      spin_called += caller && called_by(caller) == (uintptr_t)spin ? count : 0;
    } else if (address < game_lo || address >= game_hi) {
      outside += count;
      outside_called += caller >= game_lo && caller < game_hi ? count : 0;
    }
  }
  if (f) {
    fclose(f);
  }
  unlink(path);

  check(game_lo <= (uintptr_t)work && (uintptr_t)work < game_hi,
        "the game's code is the mapping holding the address given");
  check(total >= 400, "a second of CPU time gives hundreds of samples");
  check(in_spin * 10 >= total * 3, "spin takes its share of the samples");
  check(spin_called * 10 >= in_spin * 9,
        "samples in spin name the call to spin");
  check(outside * 10 >= total * 3, "memset takes its share of the samples");
  check(outside_called * 10 >= outside * 8,
        "samples in memset name the game's code that called it");
  if (failures) {
    printf("%d failed (%llu samples: %llu in spin, %llu outside)\n", failures,
           (unsigned long long)total, (unsigned long long)in_spin,
           (unsigned long long)outside);
    return 1;
  }
  printf("all passed\n");
  return 0;
}
