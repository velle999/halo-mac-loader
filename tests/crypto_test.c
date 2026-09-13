// Copyright 2026 Velle Sinclair.
//
// Simplified BSD License or GPLv3, like the rest of this tree.

// Checks hle/crypto.c: RIPEMD-160 against the algorithm's published test
// vectors, and RSA_verify against a signature made independently.
//
//   cc -o crypto_test tests/crypto_test.c hle/crypto.c && ./crypto_test

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "crypto_vectors.h"

typedef struct bignum BIGNUM;
typedef struct {
  int pad;
  int version;
  void* meth;
  void* engine;
  BIGNUM* n;
  BIGNUM* e;
} RSA;

unsigned char* RIPEMD160(const unsigned char* data, unsigned long length,
                         unsigned char* md);
BIGNUM* BN_bin2bn(const unsigned char* s, int length, BIGNUM* ret);
RSA* RSA_new(void);
void RSA_free(RSA* rsa);
int RSA_verify(int type, const unsigned char* m, unsigned int m_length,
               const unsigned char* signature, unsigned int signature_length,
               RSA* rsa);

static int failures;

static void check(int ok, const char* what) {
  if (!ok) {
    printf("FAIL: %s\n", what);
    failures++;
  }
}

static void check_ripemd160(const char* input, size_t length,
                            const char* expected) {
  unsigned char md[20];
  RIPEMD160((const unsigned char*)input, length, md);
  char hex[41];
  for (int i = 0; i < 20; i++) {
    sprintf(hex + i * 2, "%02x", md[i]);
  }
  if (strcmp(hex, expected) != 0) {
    printf("FAIL: RIPEMD160(\"%.20s%s\") = %s, not %s\n", input,
           length > 20 ? "..." : "", hex, expected);
    failures++;
  }
}

int main(void) {
  static const struct {
    const char* input;
    const char* digest;
  } kVectors[] = {
    { "", "9c1185a5c5e9fc54612808977ee8f548b2258d31" },
    { "a", "0bdc9d2d256b3ee9daae347be6f4dc835a467ffe" },
    { "abc", "8eb208f7e05d987a9b044a8e98c6b087f15a0bfc" },
    { "message digest", "5d0689ef49d2fae572b881b123a85ffa21595f36" },
    { "abcdefghijklmnopqrstuvwxyz",
      "f71c27109c692c1b56bbdceb5b9d2865b3708dbc" },
    { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
      "12a053384a9c0c88e405a06c27dcf49ada62eb2b" },
    { "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789",
      "b0e20b6e3116640286ed3a87a5713079b21f5189" },
  };
  for (size_t i = 0; i < sizeof(kVectors) / sizeof(kVectors[0]); i++) {
    check_ripemd160(kVectors[i].input, strlen(kVectors[i].input),
                    kVectors[i].digest);
  }
  char eighty[81];
  for (int i = 0; i < 80; i++) {
    eighty[i] = "1234567890"[i % 10];
  }
  eighty[80] = '\0';
  check_ripemd160(eighty, 80, "9b752e45573d4b39f4dbd3323cab82bf63326bfb");
  char* million = malloc(1000000);
  memset(million, 'a', 1000000);
  check_ripemd160(million, 1000000, "52783243c1697bdbe16d37f97f68f08325dc1528");
  free(million);

  enum { NID_ripemd160 = 117, NID_sha1 = 64 };
  RSA* rsa = RSA_new();
  rsa->n = BN_bin2bn(kModulus, sizeof(kModulus), NULL);
  rsa->e = BN_bin2bn(kExponent, sizeof(kExponent), NULL);
  check(RSA_verify(NID_ripemd160, kDigest, sizeof(kDigest), kSignature,
                   sizeof(kSignature), rsa) == 1,
        "a good signature verifies");
  unsigned char digest[sizeof(kDigest)];
  memcpy(digest, kDigest, sizeof(digest));
  digest[7] ^= 1;
  check(RSA_verify(NID_ripemd160, digest, sizeof(digest), kSignature,
                   sizeof(kSignature), rsa) == 0,
        "another digest does not");
  unsigned char signature[sizeof(kSignature)];
  memcpy(signature, kSignature, sizeof(signature));
  signature[40] ^= 0x80;
  check(RSA_verify(NID_ripemd160, kDigest, sizeof(kDigest), signature,
                   sizeof(signature), rsa) == 0,
        "a damaged signature does not");
  check(RSA_verify(NID_sha1, kDigest, sizeof(kDigest), kSignature,
                   sizeof(kSignature), rsa) == 0,
        "the digest must be the kind asked for");
  RSA_free(rsa);

  if (failures == 0) {
    printf("all passed\n");
  }
  return failures != 0;
}
