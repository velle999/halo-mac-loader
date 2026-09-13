// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Faults the game takes on its own, and how it is carried past them.
//
// Halo keeps its objects in a table of 12-byte entries whose last field
// points at the object, NULL once the object is deleted. Three of its
// functions that find an object's node matrices (0x1cd4c0, 0x1cd828 and
// 0x1cdb66 in the Universal 2.0 binary) take that pointer and read the
// object without checking it:
//
//   movl   0x8(%edx,%eax,4), %eax    ; the object, or NULL
//   movswl 0x1f2(%eax), %ecx         ; where its node matrices start
//
// The effects update checks an effect's parent object and lets the effect
// go when the object is gone. The checkpoint code's search for dangerous
// effects near a player does not check, so an effect whose parent was
// deleted since the effects last updated takes the game down with a read
// of address 0x1f2.
//
// That read goes on as if the object were empty: eax is pointed into the
// middle of a read-only block of zeros, so the node matrix the function
// returns is all zeros, and a write through it still faults and is
// reported.

#define _GNU_SOURCE

#include "game_faults.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <unistd.h>

#if defined(__i386__)

// The lookup and the read that faults, which starts kReadAt bytes in.
static const uint8_t kObjectRead[] = {
  0x8b, 0x44, 0x82, 0x08,                    // movl 0x8(%edx,%eax,4), %eax
  0x0f, 0xbf, 0x88, 0xf2, 0x01, 0x00, 0x00,  // movswl 0x1f2(%eax), %ecx
};

enum {
  kReadAt = 4,
  kReadAddress = 0x1f2,
  // A node index is at most 16 bits and a node matrix 52 bytes, so any
  // index lands within 1.7 MB of the block's middle.
  kEmptyObjectBytes = 4 << 20,
};

static void* empty_object;
static int recoveries;

// Copies |length| bytes at |address| to |out| without faulting when they
// are not all readable. Returns whether they were.
static int read_memory(uintptr_t address, void* out, size_t length) {
  struct iovec local = { out, length };
  struct iovec remote = { (void*)address, length };
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) ==
         (ssize_t)length;
}

static char* put_text(char* p, const char* text) {
  size_t length = strlen(text);
  memcpy(p, text, length);
  return p + length;
}

static char* put_number(char* p, uintptr_t value, unsigned base) {
  char digits[3 * sizeof(value)];
  int n = 0;
  do {
    digits[n++] = "0123456789abcdef"[value % base];
    value /= base;
  } while (value);
  if (base == 16) {
    p = put_text(p, "0x");
  }
  while (n) {
    *p++ = digits[--n];
  }
  return p;
}

// Says on stderr where the game read a deleted object, the first ten times
// and every thousandth after, with calls a signal handler may make.
static void report(uintptr_t eip, uintptr_t ebp) {
  int n = __sync_add_and_fetch(&recoveries, 1);
  if (n > 10 && n % 1000 != 0) {
    return;
  }
  char line[200];
  char* p = put_text(line, "hle: the game read a deleted object at ");
  p = put_number(p, eip, 16);
  uintptr_t caller;
  if (read_memory(ebp + sizeof(uintptr_t), &caller, sizeof(caller))) {
    p = put_text(p, ", called from ");
    p = put_number(p, caller, 16);
  }
  p = put_text(p, "; it goes on with an empty object (");
  p = put_number(p, n, 10);
  p = put_text(p, n == 1 ? " time)\n" : " times)\n");
  ssize_t written = write(STDERR_FILENO, line, p - line);
  (void)written;
}

int hle_recover_fault(int signum, siginfo_t* info, void* context) {
  const char* setting = getenv("HLE_RECOVER");
  if (signum != SIGSEGV || !info || !context ||
      (setting && strcmp(setting, "0") == 0)) {
    return 0;
  }
  greg_t* r = ((ucontext_t*)context)->uc_mcontext.gregs;
  uintptr_t eip = (uintptr_t)r[REG_EIP];
  uint8_t code[sizeof(kObjectRead)];
  if (r[REG_EAX] != 0 || (uintptr_t)info->si_addr != kReadAddress ||
      eip < kReadAt ||
      !read_memory(eip - kReadAt, code, sizeof(code)) ||
      memcmp(code, kObjectRead, sizeof(code)) != 0) {
    return 0;
  }
  void* block = empty_object;
  if (!block) {
    block = mmap(NULL, kEmptyObjectBytes, PROT_READ,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (block == MAP_FAILED) {
      return 0;
    }
    if (!__sync_bool_compare_and_swap(&empty_object, NULL, block)) {
      munmap(block, kEmptyObjectBytes);
      block = empty_object;
    }
  }
  r[REG_EAX] = (greg_t)((uintptr_t)block + kEmptyObjectBytes / 2);
  report(eip, (uintptr_t)r[REG_EBP]);
  return 1;
}

#else

int hle_recover_fault(int signum, siginfo_t* info, void* context) {
  return 0;
}

#endif
