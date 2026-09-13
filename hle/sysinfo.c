// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// What the BSD layer says about the machine: sysctl, mounted volumes, the
// SANE number conversions and a few libc calls glibc lacks.
//
// The answers describe an Intel Mac on 10.4.9 with this machine's CPU and
// memory. A disc is mounted when HLE_CD_PATH names a directory holding its
// contents, such as an extracted disc image; the volume takes the
// directory's name unless HLE_CD_NAME gives another.

#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <time.h>
#include <unistd.h>

#include "carbon.h"

// ---------------------------------------------------------------------------
// The user's disc

const char* hle_cd_volume_path(void) {
  static char path[PATH_MAX];
  static int checked;
  if (!checked) {
    checked = 1;
    const char* configured = getenv("HLE_CD_PATH");
    struct stat st;
    if (configured && *configured && stat(configured, &st) == 0 &&
        S_ISDIR(st.st_mode)) {
      snprintf(path, sizeof(path), "%s", configured);
      size_t len = strlen(path);
      while (len > 1 && path[len - 1] == '/') {
        path[--len] = '\0';
      }
    } else if (configured && *configured) {
      fprintf(stderr, "hle: HLE_CD_PATH %s is not a directory; no disc\n",
              configured);
    }
  }
  return path[0] ? path : NULL;
}

const char* hle_cd_volume_name(void) {
  const char* path = hle_cd_volume_path();
  if (!path) {
    return NULL;
  }
  const char* name = getenv("HLE_CD_NAME");
  if (name && *name) {
    return name;
  }
  const char* slash = strrchr(path, '/');
  return slash && slash[1] ? slash + 1 : path;
}

// ---------------------------------------------------------------------------
// Machine facts

static uint64_t memory_bytes(void) {
  struct sysinfo info;
  if (sysinfo(&info) != 0) {
    return (uint64_t)512 << 20;
  }
  return (uint64_t)info.totalram * info.mem_unit;
}

// Copies the value of the first /proc/cpuinfo line whose key is |key|.
static int cpuinfo_value(const char* key, char* out, size_t size) {
  FILE* f = fopen("/proc/cpuinfo", "r");
  if (!f) {
    return 0;
  }
  char* line = NULL;
  size_t cap = 0;
  size_t key_len = strlen(key);
  int found = 0;
  while (!found && getline(&line, &cap, f) > 0) {
    if (strncmp(line, key, key_len) != 0) {
      continue;
    }
    char* colon = strchr(line + key_len, ':');
    if (!colon) {
      continue;
    }
    char* value = colon + 1;
    while (*value == ' ' || *value == '\t') {
      value++;
    }
    value[strcspn(value, "\n")] = '\0';
    snprintf(out, size, "%s", value);
    found = 1;
  }
  free(line);
  fclose(f);
  return found;
}

static int cpu_has_flag(const char* flag) {
  char flags[8192];
  if (!cpuinfo_value("flags", flags, sizeof(flags))) {
    return 0;
  }
  size_t len = strlen(flag);
  for (char* p = flags; (p = strstr(p, flag)); p += len) {
    if ((p == flags || p[-1] == ' ') && (p[len] == ' ' || p[len] == '\0')) {
      return 1;
    }
  }
  return 0;
}

static uint64_t cpu_frequency_hz(void) {
  FILE* f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "r");
  if (f) {
    unsigned long khz = 0;
    int ok = fscanf(f, "%lu", &khz) == 1;
    fclose(f);
    if (ok && khz) {
      return (uint64_t)khz * 1000;
    }
  }
  char mhz[64];
  if (cpuinfo_value("cpu MHz", mhz, sizeof(mhz))) {
    return (uint64_t)(strtod(mhz, NULL) * 1e6);
  }
  return 0;
}

static long cpu_count(void) {
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? n : 1;
}

// ---------------------------------------------------------------------------
// sysctl

typedef struct {
  int is_text;
  uint64_t number;
  size_t size;  // 4 or 8 for numbers
  char text[256];
} sysctl_value;

static void number(sysctl_value* v, uint64_t n, size_t size) {
  v->is_text = 0;
  v->number = n;
  v->size = size;
}

static void text(sysctl_value* v, const char* s) {
  v->is_text = 1;
  snprintf(v->text, sizeof(v->text), "%s", s);
}

static int lookup_name(const char* name, sysctl_value* v) {
  uint64_t memory = memory_bytes();
  if (!strcmp(name, "hw.cputype")) {
    number(v, 7, 4);  // CPU_TYPE_X86
  } else if (!strcmp(name, "hw.cpusubtype")) {
    number(v, 4, 4);  // CPU_SUBTYPE_X86_ARCH1
  } else if (!strcmp(name, "hw.cpufrequency") ||
             !strcmp(name, "hw.cpufrequency_max") ||
             !strcmp(name, "hw.cpufrequency_min")) {
    uint64_t hz = cpu_frequency_hz();
    if (!hz) {
      return 0;
    }
    number(v, hz, 8);
  } else if (!strcmp(name, "hw.memsize")) {
    number(v, memory, 8);
  } else if (!strcmp(name, "hw.physmem") || !strcmp(name, "hw.usermem")) {
    number(v, memory > INT32_MAX ? INT32_MAX : memory, 4);
  } else if (!strcmp(name, "hw.ncpu") || !strcmp(name, "hw.activecpu") ||
             !strcmp(name, "hw.availcpu") || !strcmp(name, "hw.physicalcpu") ||
             !strcmp(name, "hw.logicalcpu")) {
    number(v, cpu_count(), 4);
  } else if (!strcmp(name, "hw.pagesize")) {
    number(v, sysconf(_SC_PAGESIZE), 4);
  } else if (!strcmp(name, "hw.byteorder")) {
    number(v, 1234, 4);
  } else if (!strcmp(name, "hw.cachelinesize")) {
    number(v, 64, 4);
  } else if (!strcmp(name, "hw.l1dcachesize")) {
    long n = sysconf(_SC_LEVEL1_DCACHE_SIZE);
    number(v, n > 0 ? n : 32768, 4);
  } else if (!strcmp(name, "hw.l2cachesize")) {
    long n = sysconf(_SC_LEVEL2_CACHE_SIZE);
    number(v, n > 0 ? n : 1 << 20, 4);
  } else if (!strcmp(name, "hw.optional.altivec")) {
    number(v, 0, 4);
  } else if (!strcmp(name, "hw.optional.mmx")) {
    number(v, cpu_has_flag("mmx"), 4);
  } else if (!strcmp(name, "hw.optional.sse")) {
    number(v, cpu_has_flag("sse"), 4);
  } else if (!strcmp(name, "hw.optional.sse2")) {
    number(v, cpu_has_flag("sse2"), 4);
  } else if (!strcmp(name, "hw.optional.sse3")) {
    number(v, cpu_has_flag("pni"), 4);
  } else if (!strcmp(name, "hw.vectorunit")) {
    number(v, cpu_has_flag("sse2"), 4);
  } else if (!strcmp(name, "hw.machine")) {
    text(v, "i386");
  } else if (!strcmp(name, "hw.model")) {
    text(v, "iMac4,1");
  } else if (!strcmp(name, "machdep.cpu.brand_string")) {
    char brand[256];
    text(v, cpuinfo_value("model name", brand, sizeof(brand)) ? brand
                                                              : "Intel");
  } else if (!strcmp(name, "kern.ostype")) {
    text(v, "Darwin");
  } else if (!strcmp(name, "kern.osrelease")) {
    text(v, "8.9.1");
  } else if (!strcmp(name, "kern.osversion")) {
    text(v, "8L2127");
  } else if (!strcmp(name, "kern.hostname")) {
    char host[256];
    text(v, gethostname(host, sizeof(host)) == 0 ? host : "localhost");
  } else {
    return 0;
  }
  return 1;
}

static int answer(const sysctl_value* v, void* oldp, size_t* oldlenp) {
  if (!oldlenp) {
    return 0;
  }
  if (v->is_text) {
    size_t len = strlen(v->text) + 1;
    if (!oldp) {
      *oldlenp = len;
      return 0;
    }
    if (*oldlenp < len) {
      memcpy(oldp, v->text, *oldlenp);
      errno = ENOMEM;
      return -1;
    }
    memcpy(oldp, v->text, len);
    *oldlenp = len;
    return 0;
  }
  if (!oldp) {
    *oldlenp = v->size;
    return 0;
  }
  if (*oldlenp >= v->size) {
    memcpy(oldp, &v->number, v->size);
    *oldlenp = v->size;
    return 0;
  }
  if (v->size == 8 && *oldlenp == 4) {
    uint32_t low = (uint32_t)v->number;
    memcpy(oldp, &low, 4);
    return 0;
  }
  errno = ENOMEM;
  return -1;
}

int sysctlbyname(const char* name, void* oldp, size_t* oldlenp, void* newp,
                 size_t newlen) {
  sysctl_value v;
  if (!name || !lookup_name(name, &v)) {
    cf_trace("sysctlbyname(%s): unknown", name ? name : "(null)");
    errno = ENOENT;
    return -1;
  }
  if (newp) {
    errno = EPERM;
    return -1;
  }
  return answer(&v, oldp, oldlenp);
}

enum {
  CTL_KERN = 1,
  CTL_HW = 6,
  KERN_OSTYPE = 1,
  KERN_OSRELEASE = 2,
  KERN_HOSTNAME = 10,
  KERN_PROC = 14,
  HW_MACHINE = 1,
  HW_MODEL = 2,
  HW_NCPU = 3,
  HW_BYTEORDER = 4,
  HW_PHYSMEM = 5,
  HW_USERMEM = 6,
  HW_PAGESIZE = 7,
  HW_CPU_FREQ = 15,
  HW_MEMSIZE = 24,
  HW_AVAILCPU = 25,
};

int sysctl(const int* mib, unsigned int count, void* oldp, size_t* oldlenp,
           void* newp, size_t newlen) {
  if (!mib || count < 2) {
    errno = EINVAL;
    return -1;
  }
  const char* name = NULL;
  if (mib[0] == CTL_KERN) {
    switch (mib[1]) {
      case KERN_OSTYPE: name = "kern.ostype"; break;
      case KERN_OSRELEASE: name = "kern.osrelease"; break;
      case KERN_HOSTNAME: name = "kern.hostname"; break;
      case KERN_PROC: {
        // The process table holds this process alone, so in particular no
        // debugger. The game searches the raw records for names and
        // assumes there is at least one.
        enum { kKinfoProcSize = 492, kCommandOffset = 163 };
        if (!oldlenp) {
          errno = EINVAL;
          return -1;
        }
        if (!oldp) {
          *oldlenp = kKinfoProcSize;
          return 0;
        }
        if (*oldlenp < kKinfoProcSize) {
          errno = ENOMEM;
          return -1;
        }
        memset(oldp, 0, kKinfoProcSize);
        memcpy((char*)oldp + kCommandOffset, "Halo", 5);
        *oldlenp = kKinfoProcSize;
        return 0;
      }
    }
  } else if (mib[0] == CTL_HW) {
    switch (mib[1]) {
      case HW_MACHINE: name = "hw.machine"; break;
      case HW_MODEL: name = "hw.model"; break;
      case HW_NCPU: name = "hw.ncpu"; break;
      case HW_BYTEORDER: name = "hw.byteorder"; break;
      case HW_PHYSMEM: name = "hw.physmem"; break;
      case HW_USERMEM: name = "hw.usermem"; break;
      case HW_PAGESIZE: name = "hw.pagesize"; break;
      case HW_MEMSIZE: name = "hw.memsize"; break;
      case HW_AVAILCPU: name = "hw.availcpu"; break;
      case HW_CPU_FREQ: {
        uint64_t hz = cpu_frequency_hz();
        sysctl_value v;
        number(&v, hz > UINT32_MAX ? UINT32_MAX : hz, 4);
        return answer(&v, oldp, oldlenp);
      }
    }
  }
  if (!name) {
    cf_trace("sysctl(%d.%d): unknown", mib[0], mib[1]);
    errno = ENOENT;
    return -1;
  }
  return sysctlbyname(name, oldp, oldlenp, newp, newlen);
}

// ---------------------------------------------------------------------------
// Mounted volumes: the startup disk and the user's disc.

enum {
  MNT_RDONLY = 0x00000001,
  MNT_NOSUID = 0x00000008,
  MNT_LOCAL = 0x00001000,
  MNT_ROOTFS = 0x00004000,
};

// struct statfs as a 10.4 i386 binary lays it out.
typedef struct {
  int16_t f_otype;
  int16_t f_oflags;
  int32_t f_bsize;
  int32_t f_iosize;
  int32_t f_blocks;
  int32_t f_bfree;
  int32_t f_bavail;
  int32_t f_files;
  int32_t f_ffree;
  int32_t f_fsid[2];
  uint32_t f_owner;
  int16_t f_reserved1;
  int16_t f_type;
  int32_t f_flags;
  int32_t f_reserved2[2];
  char f_fstypename[15];
  char f_mntonname[90];
  char f_mntfromname[90];
  char f_reserved3;
  int32_t f_reserved4[4];
} darwin_statfs;

#ifdef __i386__
_Static_assert(sizeof(darwin_statfs) == 272, "darwin_statfs");
_Static_assert(offsetof(darwin_statfs, f_mntonname) == 75, "f_mntonname");
#endif

// The BSD name the disc's partition has, for IOKit to agree with.
const char* hle_cd_bsd_name(void) {
  return "disk9s0";
}

static void fill_sizes(darwin_statfs* fs, const char* path) {
  struct statvfs sv;
  if (statvfs(path, &sv) != 0) {
    return;
  }
  uint64_t block = sv.f_frsize ? sv.f_frsize : 4096;
  // Scale to 32 bits by growing the block size, as Darwin does for big disks.
  uint64_t blocks = sv.f_blocks;
  uint64_t bfree = sv.f_bfree;
  uint64_t bavail = sv.f_bavail;
  while (blocks > INT32_MAX) {
    block *= 2;
    blocks /= 2;
    bfree /= 2;
    bavail /= 2;
  }
  fs->f_bsize = block;
  fs->f_iosize = 65536;
  fs->f_blocks = blocks;
  fs->f_bfree = bfree;
  fs->f_bavail = bavail;
  fs->f_files = sv.f_files > INT32_MAX ? INT32_MAX : sv.f_files;
  fs->f_ffree = sv.f_ffree > INT32_MAX ? INT32_MAX : sv.f_ffree;
}

int getmntinfo(darwin_statfs** out, int flags) {
  static darwin_statfs volumes[2];
  int count = 0;

  darwin_statfs* root = &volumes[count++];
  memset(root, 0, sizeof(*root));
  fill_sizes(root, hle_mac_root());
  root->f_flags = MNT_LOCAL | MNT_ROOTFS;
  root->f_type = 17;
  root->f_fsid[0] = 0x0e000002;
  root->f_owner = 0;
  strcpy(root->f_fstypename, "hfs");
  strcpy(root->f_mntonname, "/");
  strcpy(root->f_mntfromname, "/dev/disk0s2");

  const char* cd = hle_cd_volume_name();
  if (cd) {
    darwin_statfs* disc = &volumes[count++];
    memset(disc, 0, sizeof(*disc));
    fill_sizes(disc, hle_cd_volume_path());
    // A read-only disc still reports the free blocks in its volume header.
    if (disc->f_bfree == 0) {
      disc->f_bfree = disc->f_bavail = 1;
    }
    disc->f_flags = MNT_RDONLY | MNT_NOSUID | MNT_LOCAL;
    disc->f_type = 17;
    disc->f_fsid[0] = 0x0e000009;
    disc->f_owner = getuid();
    strcpy(disc->f_fstypename, "hfs");
    snprintf(disc->f_mntonname, sizeof(disc->f_mntonname), "/Volumes/%s", cd);
    snprintf(disc->f_mntfromname, sizeof(disc->f_mntfromname), "/dev/%s",
             hle_cd_bsd_name());
  }
  if (out) {
    *out = volumes;
  }
  return count;
}

// ---------------------------------------------------------------------------
// SANE conversions (fp.h)

// An 80-bit extended as 68K code stored it: sign and a 15-bit exponent,
// then a 64-bit mantissa with an explicit integer bit, in host-order words.
typedef struct {
  uint16_t exp;
  uint16_t man[4];
} extended80;

void dtox80(const double* x, extended80* out) {
  double v = *x;
  memset(out, 0, sizeof(*out));
  uint16_t sign = 0;
  if (signbit(v)) {
    sign = 0x8000;
    v = -v;
  }
  if (v == 0) {
    out->exp = sign;
    return;
  }
  if (isinf(v) || isnan(v)) {
    out->exp = sign | 0x7fff;
    out->man[0] = isnan(v) ? 0xc000 : 0x8000;
    return;
  }
  int e;
  double m = frexp(v, &e);  // v = m * 2^e with m in [0.5, 1)
  uint64_t mantissa = (uint64_t)ldexp(m, 64);
  out->exp = sign | (uint16_t)(e - 1 + 16383);
  out->man[0] = mantissa >> 48;
  out->man[1] = mantissa >> 32;
  out->man[2] = mantissa >> 16;
  out->man[3] = mantissa;
}

double x80tod(const extended80* x) {
  int exponent = (x->exp & 0x7fff) - 16383;
  uint64_t mantissa = (uint64_t)x->man[0] << 48 | (uint64_t)x->man[1] << 32 |
                      (uint64_t)x->man[2] << 16 | x->man[3];
  double v;
  if ((x->exp & 0x7fff) == 0x7fff) {
    v = (mantissa << 1) ? NAN : INFINITY;
  } else {
    v = ldexp((double)mantissa, exponent - 63);
  }
  return x->exp & 0x8000 ? -v : v;
}

#pragma pack(push, 2)
typedef struct {
  int8_t style;  // 0 counts significant digits, 1 digits after the point
  int8_t unused;
  int16_t digits;
} decform;

typedef struct {
  int8_t sgn;
  int8_t unused;
  int16_t exp;
  struct {
    uint8_t length;
    uint8_t text[36];
    uint8_t unused;
  } sig;
} decimal;
#pragma pack(pop)

// The value is (-1)^sgn * sig * 10^exp, sig a string of digits.
void num2dec(const decform* form, double x, decimal* d) {
  memset(d, 0, sizeof(*d));
  d->sgn = signbit(x) ? 1 : 0;
  x = fabs(x);
  if (isnan(x) || isinf(x)) {
    d->sig.length = 1;
    d->sig.text[0] = isnan(x) ? 'N' : 'I';
    return;
  }
  if (x == 0) {
    d->sig.length = 1;
    d->sig.text[0] = '0';
    return;
  }
  char buf[400];
  const char* digits;
  int length;
  if (form->style == 1) {
    int places = form->digits < 0 ? 0 : form->digits > 36 ? 36 : form->digits;
    snprintf(buf, sizeof(buf), "%.*f", places, x);
    char* point = strchr(buf, '.');
    if (point) {
      memmove(point, point + 1, strlen(point));
    }
    digits = buf;
    while (digits[0] == '0' && digits[1]) {
      digits++;
    }
    d->exp = -places;
    length = strlen(digits);
  } else {
    int significant = form->digits < 1 ? 1 : form->digits > 17 ? 17
                                                                : form->digits;
    snprintf(buf, sizeof(buf), "%.*e", significant - 1, x);
    char* e = strchr(buf, 'e');
    int power = e ? atoi(e + 1) : 0;
    if (e) {
      *e = '\0';
    }
    char* point = strchr(buf, '.');
    if (point) {
      memmove(point, point + 1, strlen(point));
    }
    digits = buf;
    length = strlen(digits);
    d->exp = power - (length - 1);
  }
  if (length > 36) {
    d->exp += length - 36;
    length = 36;
  }
  d->sig.length = length;
  memcpy(d->sig.text, digits, length);
}

// ---------------------------------------------------------------------------
// Odds and ends

void* reallocf(void* p, size_t size) {
  void* q = realloc(p, size);
  if (!q && size) {
    free(p);
  }
  return q;
}

// The game's OpenSSL keeps its own allocators; glibc's are fine.
int CRYPTO_set_mem_functions(void* (*m)(size_t), void* (*r)(void*, size_t),
                             void (*f)(void*)) {
  return 1;
}

#pragma pack(push, 2)
typedef struct {
  int32_t latitude;
  int32_t longitude;
  // Seconds east of GMT in the low 24 bits; 0x80 in the top byte during
  // daylight saving time.
  int32_t gmt_delta;
} MachineLocation;
#pragma pack(pop)

void ReadLocation(MachineLocation* location) {
  time_t now = time(NULL);
  struct tm local;
  localtime_r(&now, &local);
  location->latitude = 0;
  location->longitude = 0;
  location->gmt_delta = (int32_t)(local.tm_gmtoff & 0x00ffffff) |
                        (local.tm_isdst > 0 ? (int32_t)0x80000000 : 0);
}

// The game's exit, through rename.tab, saying where it was called from.
__attribute__((noreturn)) void __darwin_exit(int status) {
  fprintf(stderr, "hle: the game called exit(%d) from %p\n", status,
          __builtin_return_address(0));
  exit(status);
}
