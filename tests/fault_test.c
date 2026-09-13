// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Checks hle/game_faults.c with a copy of the game's code: its node matrix
// function, run on an object table whose entry is NULL, goes on with an
// empty object, and every other fault is left to be reported.
//
//   make tests/fault_test && tests/fault_test
//
// The copied code is i386, like the loader.

#define _GNU_SOURCE

#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../hle/game_faults.h"

static int failures;

static void check(int ok, const char* what) {
  if (!ok) {
    printf("FAIL: %s\n", what);
    failures++;
  }
}

// Halo's 0x1cd4c0, which returns object |index|'s node matrix |node|. The
// address of the object table's pointer, 0x46777c, and the offset of the
// read that faults are patched.
static const uint8_t kNodeMatrix[] = {
  0x55,                                      // pushl %ebp
  0x89, 0xe5,                                // movl %esp, %ebp
  0x0f, 0xb7, 0x45, 0x08,                    // movzwl 0x8(%ebp), %eax
  0x8b, 0x15, 0x7c, 0x77, 0x46, 0x00,        // movl 0x46777c, %edx
  0x8b, 0x52, 0x34,                          // movl 0x34(%edx), %edx
  0x8d, 0x04, 0x40,                          // leal (%eax,%eax,2), %eax
  0x8b, 0x44, 0x82, 0x08,                    // movl 0x8(%edx,%eax,4), %eax
  0x0f, 0xbf, 0x88, 0xf2, 0x01, 0x00, 0x00,  // movswl 0x1f2(%eax), %ecx
  0x01, 0xc1,                                // addl %eax, %ecx
  0x0f, 0xbf, 0x55, 0x0c,                    // movswl 0xc(%ebp), %edx
  0x8d, 0x04, 0x52,                          // leal (%edx,%edx,2), %eax
  0x8d, 0x04, 0x82,                          // leal (%edx,%eax,4), %eax
  0x8d, 0x04, 0x81,                          // leal (%ecx,%eax,4), %eax
  0x5d,                                      // popl %ebp
  0xc3,                                      // retl
};

enum {
  kTablePointerAt = 9,
  kReadOffsetAt = 26,
  kMatrixBytes = 52,
};

// The game's data array header as far as its first element, and an entry
// of its object table.
typedef struct {
  char name[32];
  uint8_t fields[0x34 - 32];
  void* first;
} data_array;

typedef struct {
  uint16_t salt;
  uint8_t flags;
  uint8_t type;
  uint16_t cluster;
  uint16_t size;
  void* address;
} object_entry;

static data_array objects;
static data_array* objects_pointer = &objects;
static object_entry entries[8];
static uint8_t object[4096];

typedef void* (*node_matrix_proc)(uint32_t index, int32_t node);

static sigjmp_buf declined_jump;
static volatile int recovered;
static volatile int declined;

static void on_fault(int signum, siginfo_t* info, void* context) {
  if (hle_recover_fault(signum, info, context)) {
    recovered++;
    return;
  }
  declined++;
  siglongjmp(declined_jump, 1);
}

// A new page holding |length| bytes of code at its start.
static void* place_code(const uint8_t* code, size_t length) {
  long page = sysconf(_SC_PAGESIZE);
  uint8_t* p = mmap(NULL, page, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) {
    perror("mmap");
    exit(1);
  }
  memcpy(p, code, length);
  mprotect(p, page, PROT_READ | PROT_EXEC);
  return p;
}

static node_matrix_proc make_node_matrix(uint32_t read_offset) {
  uint8_t code[sizeof(kNodeMatrix)];
  memcpy(code, kNodeMatrix, sizeof(code));
  uintptr_t table_pointer = (uintptr_t)&objects_pointer;
  memcpy(code + kTablePointerAt, &table_pointer, 4);
  memcpy(code + kReadOffsetAt, &read_offset, 4);
  return (node_matrix_proc)place_code(code, sizeof(code));
}

static int is_zero(const void* p, size_t length) {
  const uint8_t* bytes = p;
  for (size_t i = 0; i < length; i++) {
    if (bytes[i]) {
      return 0;
    }
  }
  return 1;
}

static void test_live_object(node_matrix_proc node_matrix) {
  recovered = declined = 0;
  void* matrix = node_matrix(5, 2);
  check(matrix == object + 0x200 + 2 * kMatrixBytes,
        "a live object's node matrix is where its header says");
  check(!recovered && !declined, "a live object's read does not fault");
}

static void test_deleted_object(node_matrix_proc node_matrix) {
  recovered = declined = 0;
  void* volatile matrix = NULL;
  if (sigsetjmp(declined_jump, 1) == 0) {
    matrix = node_matrix(6, 2);
  }
  check(recovered == 1 && declined == 0,
        "a deleted object's read goes on");
  if (!matrix) {
    return;
  }
  check(is_zero(matrix, kMatrixBytes), "the empty object's matrix is zeros");

  // The farthest nodes either way are inside the empty object.
  void* volatile last = NULL;
  void* volatile first = NULL;
  if (sigsetjmp(declined_jump, 1) == 0) {
    last = node_matrix(6, 32767);
    first = node_matrix(6, -32768);
  }
  check(last && is_zero(last, kMatrixBytes),
        "node 32767 of the empty object is zeros");
  check(first && is_zero(first, kMatrixBytes),
        "node -32768 of the empty object is zeros");

  declined = 0;
  if (sigsetjmp(declined_jump, 1) == 0) {
    ((uint8_t*)matrix)[0] = 1;
    check(0, "a write to the empty object went through");
  }
  check(declined == 1, "a write to the empty object is left to be reported");
}

static void test_other_read(void) {
  node_matrix_proc other = make_node_matrix(0x1f4);
  recovered = declined = 0;
  if (sigsetjmp(declined_jump, 1) == 0) {
    other(6, 2);
    check(0, "a read at another offset went on");
  }
  check(declined == 1 && recovered == 0,
        "a read at another offset is left to be reported");
}

// The faulting read at the start of a page whose previous page cannot be
// read: the lookup before it cannot be compared, which must not fault
// again.
static void test_unreadable_lookup(void) {
  long page = sysconf(_SC_PAGESIZE);
  uint8_t* pages = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (pages == MAP_FAILED) {
    perror("mmap");
    exit(1);
  }
  static const uint8_t read_and_return[] = {
    0x0f, 0xbf, 0x88, 0xf2, 0x01, 0x00, 0x00,  // movswl 0x1f2(%eax), %ecx
    0xc3,                                      // retl
  };
  memcpy(pages + page, read_and_return, sizeof(read_and_return));
  mprotect(pages, page, PROT_NONE);
  mprotect(pages + page, page, PROT_READ | PROT_EXEC);

  static uintptr_t target;
  target = (uintptr_t)(pages + page);
  uint8_t jump[] = {
    0x31, 0xc0,                    // xorl %eax, %eax
    0xff, 0x25, 0, 0, 0, 0,        // jmp *target
  };
  uintptr_t target_address = (uintptr_t)&target;
  memcpy(jump + 4, &target_address, 4);
  void (*proc)(void) = (void (*)(void))place_code(jump, sizeof(jump));

  recovered = declined = 0;
  if (sigsetjmp(declined_jump, 1) == 0) {
    proc();
    check(0, "a read with no readable lookup before it went on");
  }
  check(declined == 1 && recovered == 0,
        "a read with no readable lookup before it is left to be reported");
}

static void test_switched_off(node_matrix_proc node_matrix) {
  setenv("HLE_RECOVER", "0", 1);
  recovered = declined = 0;
  if (sigsetjmp(declined_jump, 1) == 0) {
    node_matrix(6, 2);
    check(0, "a deleted object's read went on with HLE_RECOVER=0");
  }
  check(declined == 1 && recovered == 0,
        "HLE_RECOVER=0 leaves a deleted object's read to be reported");
  unsetenv("HLE_RECOVER");
}

int main(void) {
  if (sizeof(void*) != 4 || sizeof(object_entry) != 12) {
    printf("FAIL: the copied code is i386\n");
    return 1;
  }
  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_sigaction = on_fault;
  action.sa_flags = SA_SIGINFO;
  sigemptyset(&action.sa_mask);
  sigaction(SIGSEGV, &action, NULL);

  objects.first = entries;
  entries[5].address = object;
  int16_t nodes_at = 0x200;
  memcpy(object + 0x1f2, &nodes_at, sizeof(nodes_at));
  entries[6].address = NULL;

  node_matrix_proc node_matrix = make_node_matrix(0x1f2);
  test_live_object(node_matrix);
  test_deleted_object(node_matrix);
  test_other_read();
  test_unreadable_lookup();
  test_switched_off(node_matrix);

  if (failures) {
    printf("%d failed\n", failures);
    return 1;
  }
  printf("all passed\n");
  return 0;
}
