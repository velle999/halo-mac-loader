// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// The corner of OpenSSL 0.9.7 the game uses to check signed data:
// RIPEMD-160, and RSA verification of PKCS #1 v1.5 signatures.
//
// The game builds its public key itself, storing BN_bin2bn's results in the
// n and e fields of the RSA structure RSA_new returns, so that structure
// keeps 0.9.7's layout up to those fields. BIGNUMs are private. Nothing
// here depends on the rest of the library, so tests/crypto_test.c builds it
// on its own.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// RIPEMD-160

// 96 bytes, as 0.9.7's RIPEMD160_CTX: the game allocates it.
typedef struct {
  uint32_t h[5];
  uint32_t low_bits;
  uint32_t high_bits;
  uint8_t block[64];
  int32_t used;
} RIPEMD160_CTX;

static uint32_t rol(uint32_t x, int n) {
  return x << n | x >> (32 - n);
}

static uint32_t f(int j, uint32_t x, uint32_t y, uint32_t z) {
  switch (j / 16) {
    case 0: return x ^ y ^ z;
    case 1: return (x & y) | (~x & z);
    case 2: return (x | ~y) ^ z;
    case 3: return (x & z) | (y & ~z);
    default: return x ^ (y | ~z);
  }
}

static const uint32_t kLeftK[5] = { 0x00000000, 0x5A827999, 0x6ED9EBA1,
                                    0x8F1BBCDC, 0xA953FD4E };
static const uint32_t kRightK[5] = { 0x50A28BE6, 0x5C4DD124, 0x6D703EF3,
                                     0x7A6D76E9, 0x00000000 };

static const uint8_t kLeftR[80] = {
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
  7, 4, 13, 1, 10, 6, 15, 3, 12, 0, 9, 5, 2, 14, 11, 8,
  3, 10, 14, 4, 9, 15, 8, 1, 2, 7, 0, 6, 13, 11, 5, 12,
  1, 9, 11, 10, 0, 8, 12, 4, 13, 3, 7, 15, 14, 5, 6, 2,
  4, 0, 5, 9, 7, 12, 2, 10, 14, 1, 3, 8, 11, 6, 15, 13,
};

static const uint8_t kRightR[80] = {
  5, 14, 7, 0, 9, 2, 11, 4, 13, 6, 15, 8, 1, 10, 3, 12,
  6, 11, 3, 7, 0, 13, 5, 10, 14, 15, 8, 12, 4, 9, 1, 2,
  15, 5, 1, 3, 7, 14, 6, 9, 11, 8, 12, 2, 10, 0, 4, 13,
  8, 6, 4, 1, 3, 11, 15, 0, 5, 12, 2, 13, 9, 7, 10, 14,
  12, 15, 10, 4, 1, 5, 8, 7, 6, 2, 13, 14, 0, 3, 9, 11,
};

static const uint8_t kLeftS[80] = {
  11, 14, 15, 12, 5, 8, 7, 9, 11, 13, 14, 15, 6, 7, 9, 8,
  7, 6, 8, 13, 11, 9, 7, 15, 7, 12, 15, 9, 11, 7, 13, 12,
  11, 13, 6, 7, 14, 9, 13, 15, 14, 8, 13, 6, 5, 12, 7, 5,
  11, 12, 14, 15, 14, 15, 9, 8, 9, 14, 5, 6, 8, 6, 5, 12,
  9, 15, 5, 11, 6, 8, 13, 12, 5, 12, 13, 14, 11, 8, 5, 6,
};

static const uint8_t kRightS[80] = {
  8, 9, 9, 11, 13, 15, 15, 5, 7, 7, 8, 11, 14, 14, 12, 6,
  9, 13, 15, 7, 12, 8, 9, 11, 7, 7, 12, 7, 6, 15, 13, 11,
  9, 7, 15, 11, 8, 6, 6, 14, 12, 13, 5, 14, 13, 13, 7, 5,
  15, 5, 8, 11, 14, 14, 6, 14, 6, 9, 12, 9, 12, 5, 15, 8,
  8, 5, 12, 9, 12, 5, 14, 6, 8, 13, 6, 5, 15, 13, 11, 11,
};

static void compress(uint32_t* h, const uint8_t* block) {
  uint32_t x[16];
  for (int i = 0; i < 16; i++) {
    const uint8_t* b = block + i * 4;
    x[i] = b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
  }
  uint32_t al = h[0], bl = h[1], cl = h[2], dl = h[3], el = h[4];
  uint32_t ar = h[0], br = h[1], cr = h[2], dr = h[3], er = h[4];
  for (int j = 0; j < 80; j++) {
    uint32_t t = rol(al + f(j, bl, cl, dl) + x[kLeftR[j]] + kLeftK[j / 16],
                     kLeftS[j]) + el;
    al = el;
    el = dl;
    dl = rol(cl, 10);
    cl = bl;
    bl = t;
    t = rol(ar + f(79 - j, br, cr, dr) + x[kRightR[j]] + kRightK[j / 16],
            kRightS[j]) + er;
    ar = er;
    er = dr;
    dr = rol(cr, 10);
    cr = br;
    br = t;
  }
  uint32_t t = h[1] + cl + dr;
  h[1] = h[2] + dl + er;
  h[2] = h[3] + el + ar;
  h[3] = h[4] + al + br;
  h[4] = h[0] + bl + cr;
  h[0] = t;
}

int RIPEMD160_Init(RIPEMD160_CTX* c) {
  static const uint32_t kInitial[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE,
                                        0x10325476, 0xC3D2E1F0 };
  memcpy(c->h, kInitial, sizeof(kInitial));
  c->low_bits = c->high_bits = 0;
  c->used = 0;
  return 1;
}

int RIPEMD160_Update(RIPEMD160_CTX* c, const void* data, unsigned long length) {
  const uint8_t* p = data;
  uint32_t bits = (uint32_t)length << 3;
  if (c->low_bits + bits < c->low_bits) {
    c->high_bits++;
  }
  c->low_bits += bits;
  c->high_bits += (uint32_t)(length >> 29);
  while (length > 0) {
    size_t take = 64 - c->used;
    if (take > length) {
      take = length;
    }
    memcpy(c->block + c->used, p, take);
    c->used += take;
    p += take;
    length -= take;
    if (c->used == 64) {
      compress(c->h, c->block);
      c->used = 0;
    }
  }
  return 1;
}

int RIPEMD160_Final(unsigned char* md, RIPEMD160_CTX* c) {
  uint32_t low = c->low_bits;
  uint32_t high = c->high_bits;
  uint8_t pad = 0x80;
  RIPEMD160_Update(c, &pad, 1);
  uint8_t zero = 0;
  while (c->used != 56) {
    RIPEMD160_Update(c, &zero, 1);
  }
  uint8_t length[8];
  for (int i = 0; i < 4; i++) {
    length[i] = low >> (8 * i);
    length[4 + i] = high >> (8 * i);
  }
  RIPEMD160_Update(c, length, 8);
  for (int i = 0; i < 20; i++) {
    md[i] = c->h[i / 4] >> (8 * (i % 4));
  }
  return 1;
}

unsigned char* RIPEMD160(const unsigned char* data, unsigned long length,
                         unsigned char* md) {
  static unsigned char buffer[20];
  RIPEMD160_CTX c;
  RIPEMD160_Init(&c);
  RIPEMD160_Update(&c, data, length);
  RIPEMD160_Final(md ? md : buffer, &c);
  return md ? md : buffer;
}

// ---------------------------------------------------------------------------
// Big numbers: little-endian 32-bit limbs, enough for 4096-bit keys.

enum {
  kMaxLimbs = 130,
  kBignumMagic = 0x42494721,
};

typedef struct {
  uint32_t magic;
  int limbs;
  uint32_t d[kMaxLimbs];
} BIGNUM;

static void trim(BIGNUM* a) {
  while (a->limbs > 0 && a->d[a->limbs - 1] == 0) {
    a->limbs--;
  }
}

static int compare(const BIGNUM* a, const BIGNUM* b) {
  if (a->limbs != b->limbs) {
    return a->limbs < b->limbs ? -1 : 1;
  }
  for (int i = a->limbs - 1; i >= 0; i--) {
    if (a->d[i] != b->d[i]) {
      return a->d[i] < b->d[i] ? -1 : 1;
    }
  }
  return 0;
}

// a -= b, where a >= b.
static void subtract(BIGNUM* a, const BIGNUM* b) {
  uint64_t borrow = 0;
  for (int i = 0; i < a->limbs; i++) {
    uint64_t sub = (uint64_t)(i < b->limbs ? b->d[i] : 0) + borrow;
    borrow = a->d[i] < sub;
    a->d[i] = (uint32_t)(a->d[i] - sub);
  }
  trim(a);
}

// r = a * b mod m.
static void mul_mod(BIGNUM* r, const BIGNUM* a, const BIGNUM* b,
                    const BIGNUM* m) {
  uint32_t product[2 * kMaxLimbs];
  int n = a->limbs + b->limbs;
  memset(product, 0, sizeof(uint32_t) * n);
  for (int i = 0; i < a->limbs; i++) {
    uint64_t carry = 0;
    for (int j = 0; j < b->limbs; j++) {
      uint64_t t = (uint64_t)a->d[i] * b->d[j] + product[i + j] + carry;
      product[i + j] = (uint32_t)t;
      carry = t >> 32;
    }
    product[i + b->limbs] = (uint32_t)carry;
  }
  // Long division one bit at a time: the remainder stays below 2m.
  BIGNUM rem = { kBignumMagic, 0, { 0 } };
  for (int bit = n * 32 - 1; bit >= 0; bit--) {
    uint32_t carry = (product[bit / 32] >> (bit % 32)) & 1;
    for (int i = 0; i < rem.limbs; i++) {
      uint32_t next = rem.d[i] >> 31;
      rem.d[i] = rem.d[i] << 1 | carry;
      carry = next;
    }
    if (carry) {
      rem.d[rem.limbs++] = carry;
    }
    if (compare(&rem, m) >= 0) {
      subtract(&rem, m);
    }
  }
  *r = rem;
}

static int bit_count(const BIGNUM* a) {
  if (a->limbs == 0) {
    return 0;
  }
  uint32_t top = a->d[a->limbs - 1];
  int bits = (a->limbs - 1) * 32;
  while (top) {
    bits++;
    top >>= 1;
  }
  return bits;
}

// r = base^exp mod m.
static void exp_mod(BIGNUM* r, const BIGNUM* base, const BIGNUM* exp,
                    const BIGNUM* m) {
  BIGNUM result = { kBignumMagic, 1, { 1 } };
  BIGNUM b;
  BIGNUM one = { kBignumMagic, 1, { 1 } };
  mul_mod(&b, base, &one, m);
  for (int bit = bit_count(exp) - 1; bit >= 0; bit--) {
    mul_mod(&result, &result, &result, m);
    if ((exp->d[bit / 32] >> (bit % 32)) & 1) {
      mul_mod(&result, &result, &b, m);
    }
  }
  *r = result;
}

BIGNUM* BN_bin2bn(const unsigned char* s, int length, BIGNUM* ret) {
  if (length < 0 || length > kMaxLimbs * 4 / 2) {
    return NULL;
  }
  BIGNUM* a = ret ? ret : malloc(sizeof(BIGNUM));
  if (!a) {
    return NULL;
  }
  memset(a, 0, sizeof(*a));
  a->magic = kBignumMagic;
  for (int i = 0; i < length; i++) {
    int from_end = length - 1 - i;
    a->d[from_end / 4] |= (uint32_t)s[i] << (8 * (from_end % 4));
  }
  a->limbs = (length + 3) / 4;
  trim(a);
  return a;
}

void BN_free(BIGNUM* a) {
  if (a && a->magic == kBignumMagic) {
    a->magic = 0;
    free(a);
  }
}

// ---------------------------------------------------------------------------
// RSA

// The start of 0.9.7's struct rsa_st; the game writes n and e.
typedef struct {
  int32_t pad;
  int32_t version;
  void* meth;
  void* engine;
  BIGNUM* n;
  BIGNUM* e;
  BIGNUM* d;
  BIGNUM* p;
  BIGNUM* q;
  BIGNUM* dmp1;
  BIGNUM* dmq1;
  BIGNUM* iqmp;
  uint8_t rest[64];
} RSA;

RSA* RSA_new(void) {
  return calloc(1, sizeof(RSA));
}

void RSA_free(RSA* rsa) {
  if (!rsa) {
    return;
  }
  BN_free(rsa->n);
  BN_free(rsa->e);
  free(rsa);
}

enum {
  NID_md5 = 4,
  NID_sha1 = 64,
  NID_md5_sha1 = 114,
  NID_ripemd160 = 117,
};

// DER DigestInfo prefixes, with and without the NULL parameters some signers
// leave out.
static const struct {
  int type;
  size_t length;
  const uint8_t prefix[19];
} kDigestInfo[] = {
  { NID_ripemd160, 15, { 0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x24, 0x03,
                         0x02, 0x01, 0x05, 0x00, 0x04, 0x14 } },
  { NID_ripemd160, 13, { 0x30, 0x1f, 0x30, 0x07, 0x06, 0x05, 0x2b, 0x24, 0x03,
                         0x02, 0x01, 0x04, 0x14 } },
  { NID_sha1, 15, { 0x30, 0x21, 0x30, 0x09, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02,
                    0x1a, 0x05, 0x00, 0x04, 0x14 } },
  { NID_sha1, 13, { 0x30, 0x1f, 0x30, 0x07, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02,
                    0x1a, 0x04, 0x14 } },
  { NID_md5, 18, { 0x30, 0x20, 0x30, 0x0c, 0x06, 0x08, 0x2a, 0x86, 0x48, 0x86,
                   0xf7, 0x0d, 0x02, 0x05, 0x05, 0x00, 0x04, 0x10 } },
};

// 1 when |signature| is |rsa|'s PKCS #1 v1.5 signature of the digest |m|.
int RSA_verify(int type, const unsigned char* m, unsigned int m_length,
               const unsigned char* signature, unsigned int signature_length,
               RSA* rsa) {
  if (!rsa || !rsa->n || !rsa->e || rsa->n->magic != kBignumMagic ||
      rsa->e->magic != kBignumMagic) {
    return 0;
  }
  const BIGNUM* n = rsa->n;
  unsigned int k = (bit_count(n) + 7) / 8;
  if (signature_length != k || k < 11 || k > kMaxLimbs * 4 / 2) {
    return 0;
  }
  BIGNUM s;
  BN_bin2bn(signature, signature_length, &s);
  if (compare(&s, n) >= 0) {
    return 0;
  }
  BIGNUM em_number;
  exp_mod(&em_number, &s, rsa->e, n);

  uint8_t em[kMaxLimbs * 4];
  for (unsigned int i = 0; i < k; i++) {
    unsigned int from_end = k - 1 - i;
    unsigned int limb = from_end / 4;
    em[i] = limb < (unsigned int)em_number.limbs
                ? em_number.d[limb] >> (8 * (from_end % 4))
                : 0;
  }
  // 00 01 FF...FF 00 T, with at least eight FF bytes.
  if (em[0] != 0x00 || em[1] != 0x01) {
    return 0;
  }
  unsigned int i = 2;
  while (i < k && em[i] == 0xFF) {
    i++;
  }
  if (i < 10 || i >= k || em[i] != 0x00) {
    return 0;
  }
  const uint8_t* t = em + i + 1;
  unsigned int t_length = k - i - 1;
  if (type == NID_md5_sha1) {
    return t_length == m_length && m_length == 36 &&
           memcmp(t, m, m_length) == 0;
  }
  for (size_t d = 0; d < sizeof(kDigestInfo) / sizeof(kDigestInfo[0]); d++) {
    if (kDigestInfo[d].type == type &&
        t_length == kDigestInfo[d].length + m_length &&
        memcmp(t, kDigestInfo[d].prefix, kDigestInfo[d].length) == 0) {
      return memcmp(t + kDigestInfo[d].length, m, m_length) == 0;
    }
  }
  return 0;
}
