// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// BSD sockets with Darwin's layouts and numbers.
//
// glibc has every one of these calls, but a Darwin sockaddr starts with a
// length byte and a one-byte family, SOL_SOCKET and the SO_ and IP_ options
// have other values, message flags and ioctl requests are numbered
// differently, and errno values from 35 up disagree. rename.tab sends the
// game's calls here, and failures set errno to the Darwin value the game
// compares against.

#define _GNU_SOURCE

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cf.h"

// ---------------------------------------------------------------------------
// errno

static const struct {
  int linux_value;
  int darwin_value;
} kErrno[] = {
  { EAGAIN, 35 },         { EDEADLK, 11 },        { ENAMETOOLONG, 63 },
  { ENOLCK, 77 },         { ENOSYS, 78 },         { ENOTEMPTY, 66 },
  { ELOOP, 62 },          { EOVERFLOW, 84 },      { EILSEQ, 92 },
  { ENOTSOCK, 38 },       { EDESTADDRREQ, 39 },   { EMSGSIZE, 40 },
  { EPROTOTYPE, 41 },     { ENOPROTOOPT, 42 },    { EPROTONOSUPPORT, 43 },
  { ESOCKTNOSUPPORT, 44 }, { EOPNOTSUPP, 102 },   { EPFNOSUPPORT, 46 },
  { EAFNOSUPPORT, 47 },   { EADDRINUSE, 48 },     { EADDRNOTAVAIL, 49 },
  { ENETDOWN, 50 },       { ENETUNREACH, 51 },    { ENETRESET, 52 },
  { ECONNABORTED, 53 },   { ECONNRESET, 54 },     { ENOBUFS, 55 },
  { EISCONN, 56 },        { ENOTCONN, 57 },       { ESHUTDOWN, 58 },
  { ETOOMANYREFS, 59 },   { ETIMEDOUT, 60 },      { ECONNREFUSED, 61 },
  { EHOSTDOWN, 64 },      { EHOSTUNREACH, 65 },   { EALREADY, 37 },
  { EINPROGRESS, 36 },    { ESTALE, 70 },         { EDQUOT, 69 },
  { ECANCELED, 89 },
};

// Returns -1 with errno holding its Darwin value.
static int fail(void) {
  int e = errno;
  for (size_t i = 0; i < sizeof(kErrno) / sizeof(kErrno[0]); i++) {
    if (kErrno[i].linux_value == e) {
      errno = kErrno[i].darwin_value;
      break;
    }
  }
  return -1;
}

static int fail_with(int darwin_errno) {
  errno = darwin_errno;
  return -1;
}

enum {
  kDarwinEINVAL = 22,
  kDarwinENOPROTOOPT = 42,
  kDarwinENOTTY = 25,
};

// ---------------------------------------------------------------------------
// Addresses

enum {
  kDarwinAF_INET6 = 30,
};

static int family_to_linux(int family) {
  return family == kDarwinAF_INET6 ? AF_INET6 : family;
}

static int family_to_darwin(int family) {
  return family == AF_INET6 ? kDarwinAF_INET6 : family;
}

// Copies a Darwin address into |out|, which is laid out for Linux.
static int address_in(const void* darwin, socklen_t length,
                      struct sockaddr_storage* out) {
  if (!darwin || length < 2 || length > sizeof(*out)) {
    return 0;
  }
  memcpy(out, darwin, length);
  out->ss_family = family_to_linux(((const uint8_t*)darwin)[1]);
  return 1;
}

// Copies a Linux address out to the game's buffer of |*capacity| bytes,
// setting |*capacity| to the address's full length as the call does.
static void address_out(const struct sockaddr_storage* in, socklen_t length,
                        void* darwin, socklen_t* capacity) {
  if (!darwin || !capacity) {
    return;
  }
  uint8_t bytes[sizeof(struct sockaddr_storage)];
  memcpy(bytes, in, length);
  bytes[0] = length > 255 ? 255 : length;
  bytes[1] = family_to_darwin(in->ss_family);
  memcpy(darwin, bytes, length < *capacity ? length : *capacity);
  *capacity = length;
}

// ---------------------------------------------------------------------------
// Message flags

static int flags_to_linux(int flags) {
  static const int kMap[][2] = {
    { 0x1, MSG_OOB },     { 0x2, MSG_PEEK },     { 0x4, MSG_DONTROUTE },
    { 0x8, MSG_EOR },     { 0x10, MSG_TRUNC },   { 0x20, MSG_CTRUNC },
    { 0x40, MSG_WAITALL }, { 0x80, MSG_DONTWAIT },
  };
  int out = 0;
  for (size_t i = 0; i < sizeof(kMap) / sizeof(kMap[0]); i++) {
    if (flags & kMap[i][0]) {
      out |= kMap[i][1];
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Calls

int __darwin_bind(int fd, const void* address, socklen_t length) {
  struct sockaddr_storage a;
  if (!address_in(address, length, &a)) {
    return fail_with(kDarwinEINVAL);
  }
  return bind(fd, (struct sockaddr*)&a, length) == 0 ? 0 : fail();
}

int __darwin_connect(int fd, const void* address, socklen_t length) {
  struct sockaddr_storage a;
  if (!address_in(address, length, &a)) {
    return fail_with(kDarwinEINVAL);
  }
  return connect(fd, (struct sockaddr*)&a, length) == 0 ? 0 : fail();
}

int __darwin_getsockname(int fd, void* address, socklen_t* length) {
  struct sockaddr_storage a;
  socklen_t len = sizeof(a);
  if (getsockname(fd, (struct sockaddr*)&a, &len) != 0) {
    return fail();
  }
  address_out(&a, len, address, length);
  return 0;
}

ssize_t __darwin_recvfrom(int fd, void* buffer, size_t size, int flags,
                          void* address, socklen_t* length) {
  struct sockaddr_storage a;
  socklen_t len = sizeof(a);
  ssize_t n = recvfrom(fd, buffer, size, flags_to_linux(flags),
                       address ? (struct sockaddr*)&a : NULL,
                       address ? &len : NULL);
  if (n < 0) {
    return fail();
  }
  if (address) {
    address_out(&a, len, address, length);
  }
  return n;
}

// A Mac never raises SIGPIPE for a socket that asked not to, and games ask;
// here no send raises it.
ssize_t __darwin_sendto(int fd, const void* buffer, size_t size, int flags,
                        const void* address, socklen_t length) {
  struct sockaddr_storage a;
  if (address && !address_in(address, length, &a)) {
    return fail_with(kDarwinEINVAL);
  }
  ssize_t n = sendto(fd, buffer, size, flags_to_linux(flags) | MSG_NOSIGNAL,
                     address ? (struct sockaddr*)&a : NULL,
                     address ? length : 0);
  return n < 0 ? fail() : n;
}

ssize_t __darwin_recv(int fd, void* buffer, size_t size, int flags) {
  ssize_t n = recv(fd, buffer, size, flags_to_linux(flags));
  return n < 0 ? fail() : n;
}

ssize_t __darwin_send(int fd, const void* buffer, size_t size, int flags) {
  ssize_t n = send(fd, buffer, size, flags_to_linux(flags) | MSG_NOSIGNAL);
  return n < 0 ? fail() : n;
}

enum {
  kDarwinSOL_SOCKET = 0xffff,
  kDarwinSO_NOSIGPIPE = 0x1022,
  kDarwinTCP_KEEPALIVE = 0x10,
};

// Translates a socket option. Returns 0 when Linux has no equivalent; |*skip|
// is set for options that need no counterpart at all.
static int option_to_linux(int level, int name, int* linux_level,
                           int* linux_name, int* skip) {
  static const int kSocket[][2] = {
    { 0x0001, SO_DEBUG },     { 0x0004, SO_REUSEADDR },  { 0x0008, SO_KEEPALIVE },
    { 0x0010, SO_DONTROUTE }, { 0x0020, SO_BROADCAST },  { 0x0080, SO_LINGER },
    { 0x0100, SO_OOBINLINE }, { 0x0200, SO_REUSEPORT },  { 0x1001, SO_SNDBUF },
    { 0x1002, SO_RCVBUF },    { 0x1003, SO_SNDLOWAT },   { 0x1004, SO_RCVLOWAT },
    { 0x1005, SO_SNDTIMEO },  { 0x1006, SO_RCVTIMEO },   { 0x1007, SO_ERROR },
    { 0x1008, SO_TYPE },
  };
  static const int kIp[][2] = {
    { 1, IP_OPTIONS },         { 2, IP_HDRINCL },         { 3, IP_TOS },
    { 4, IP_TTL },             { 9, IP_MULTICAST_IF },    { 10, IP_MULTICAST_TTL },
    { 11, IP_MULTICAST_LOOP }, { 12, IP_ADD_MEMBERSHIP }, { 13, IP_DROP_MEMBERSHIP },
  };
  *skip = 0;
  if (level == kDarwinSOL_SOCKET) {
    *linux_level = SOL_SOCKET;
    if (name == kDarwinSO_NOSIGPIPE) {
      *skip = 1;
      return 1;
    }
    for (size_t i = 0; i < sizeof(kSocket) / sizeof(kSocket[0]); i++) {
      if (kSocket[i][0] == name) {
        *linux_name = kSocket[i][1];
        return 1;
      }
    }
    return 0;
  }
  if (level == IPPROTO_IP) {
    *linux_level = IPPROTO_IP;
    for (size_t i = 0; i < sizeof(kIp) / sizeof(kIp[0]); i++) {
      if (kIp[i][0] == name) {
        *linux_name = kIp[i][1];
        return 1;
      }
    }
    return 0;
  }
  if (level == IPPROTO_TCP) {
    *linux_level = IPPROTO_TCP;
    if (name == TCP_NODELAY || name == TCP_MAXSEG) {
      *linux_name = name;
      return 1;
    }
    if (name == kDarwinTCP_KEEPALIVE) {
      *linux_name = TCP_KEEPIDLE;
      return 1;
    }
    return 0;
  }
  return 0;
}

int __darwin_setsockopt(int fd, int level, int name, const void* value,
                        socklen_t length) {
  int linux_level, linux_name, skip;
  if (!option_to_linux(level, name, &linux_level, &linux_name, &skip)) {
    cf_trace("setsockopt(level %#x, option %#x): no Linux equivalent", level,
             name);
    return fail_with(kDarwinENOPROTOOPT);
  }
  if (skip) {
    return 0;
  }
  return setsockopt(fd, linux_level, linux_name, value, length) == 0 ? 0
                                                                     : fail();
}

int __darwin_getsockopt(int fd, int level, int name, void* value,
                        socklen_t* length) {
  int linux_level, linux_name, skip;
  if (!option_to_linux(level, name, &linux_level, &linux_name, &skip)) {
    return fail_with(kDarwinENOPROTOOPT);
  }
  if (skip) {
    if (value && length && *length >= sizeof(int)) {
      *(int*)value = 1;
      *length = sizeof(int);
    }
    return 0;
  }
  if (getsockopt(fd, linux_level, linux_name, value, length) != 0) {
    return fail();
  }
  if (linux_level == SOL_SOCKET && linux_name == SO_ERROR && value) {
    int* err = value;
    int saved = errno;
    errno = *err;
    fail();
    *err = *err ? errno : 0;
    errno = saved;
  }
  return 0;
}

enum {
  kDarwinFIONBIO = (int)0x8004667e,
  kDarwinFIONREAD = 0x4004667f,
  kDarwinFIOASYNC = (int)0x8004667d,
  kDarwinFIOCLEX = 0x20006601,
  kDarwinFIONCLEX = 0x20006602,
};

int __darwin_ioctl(int fd, unsigned long request, ...) {
  va_list ap;
  va_start(ap, request);
  void* arg = va_arg(ap, void*);
  va_end(ap);
  unsigned long linux_request;
  switch ((int)request) {
    case kDarwinFIONBIO: linux_request = FIONBIO; break;
    case kDarwinFIONREAD: linux_request = FIONREAD; break;
    case kDarwinFIOASYNC: linux_request = FIOASYNC; break;
    case kDarwinFIOCLEX: linux_request = FIOCLEX; break;
    case kDarwinFIONCLEX: linux_request = FIONCLEX; break;
    default:
      cf_trace("ioctl(%d, %#lx): not translated", fd, request);
      return fail_with(kDarwinENOTTY);
  }
  return ioctl(fd, linux_request, arg) == 0 ? 0 : fail();
}
