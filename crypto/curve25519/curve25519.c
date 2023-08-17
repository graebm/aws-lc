/* Copyright (c) 2020, Google Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

// Some of this code is taken from the ref10 version of Ed25519 in SUPERCOP
// 20141124 (http://bench.cr.yp.to/supercop.html). That code is released as
// public domain. Other parts have been replaced to call into code generated by
// Fiat (https://github.com/mit-plv/fiat-crypto) in //third_party/fiat.
//
// The field functions are shared by Ed25519 and X25519 where possible.

#include <openssl/curve25519.h>

#include <string.h>

#include <openssl/mem.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include "internal.h"
#include "../internal.h"
#include "../fipsmodule/cpucap/internal.h"

// If (1) x86_64 or aarch64, (2) linux or apple, and (3) OPENSSL_NO_ASM is not
// set, s2n-bignum path is capable.
#if ((defined(OPENSSL_X86_64) &&                                               \
          !defined(MY_ASSEMBLER_IS_TOO_OLD_FOR_AVX)) ||                        \
      defined(OPENSSL_AARCH64)) &&                                             \
     (defined(OPENSSL_LINUX) || defined(OPENSSL_APPLE)) &&                     \
     !defined(OPENSSL_NO_ASM)
#include "../../third_party/s2n-bignum/include/s2n-bignum_aws-lc.h"
#define CURVE25519_S2N_BIGNUM_CAPABLE
#endif


OPENSSL_INLINE int x25519_s2n_bignum_capable(void) {
#if defined(CURVE25519_S2N_BIGNUM_CAPABLE)
  return 1;
#else
  return 0;
#endif
}

// Stub functions if implementations are not compiled.
// These functions have to abort, otherwise we risk applications assuming they
// did work without actually doing anything.

#if !defined(CURVE25519_S2N_BIGNUM_CAPABLE)

void curve25519_x25519_byte(uint8_t res[32], const uint8_t scalar[32],
  const uint8_t point[32]);
void curve25519_x25519_byte_alt(uint8_t res[32], const uint8_t scalar[32],
  const uint8_t point[32]);
void curve25519_x25519base_byte(uint8_t res[32], const uint8_t scalar[32]);
void curve25519_x25519base_byte_alt(uint8_t res[32], const uint8_t scalar[32]);

void curve25519_x25519_byte(uint8_t res[32], const uint8_t scalar[32],
  const uint8_t point[32]) {
  abort();
}
void curve25519_x25519_byte_alt(uint8_t res[32], const uint8_t scalar[32],
  const uint8_t point[32]) {
  abort();
}
void curve25519_x25519base_byte(uint8_t res[32], const uint8_t scalar[32]) {
  abort();
}
void curve25519_x25519base_byte_alt(uint8_t res[32], const uint8_t scalar[32]) {
  abort();
}

#endif // !defined(CURVE25519_S2N_BIGNUM_CAPABLE)


// Run-time detection for each implementation

OPENSSL_INLINE int x25519_s2n_bignum_alt_capable(void);
OPENSSL_INLINE int x25519_s2n_bignum_no_alt_capable(void);

// For aarch64, |x25519_s2n_bignum_alt_capable| returns 1 if we categorize the
// CPU as a CPU having a wide multiplier (i.e. "higher" throughput). CPUs with
// this feature are e.g.: AWS Graviton 3 and Apple M1. Return 0 otherwise, so we
// don't match CPUs without wide multipliers.
//
// For x86_64, |x25519_s2n_bignum_alt_capable| always returns 1. If x25519
// s2n-bignum capable, the x86_64 s2n-bignum-alt version should be supported on
// pretty much any x86_64 CPU.
//
// For all other architectures, return 0.
OPENSSL_INLINE int x25519_s2n_bignum_alt_capable(void) {
#if defined(OPENSSL_X86_64)
  return 1;
#elif defined(OPENSSL_AARCH64)
  if (CRYPTO_is_ARMv8_wide_multiplier_capable() == 1) {
    return 1;
  } else {
    return 0;
  }
#else
  return 0;
#endif
}

// For aarch64, |x25519_s2n_bignum_no_alt_capable| always returns 1. If x25519
// s2n-bignum capable, the Armv8 s2n-bignum-alt version should be supported on
// pretty much any Armv8 CPU.
//
// For x86_64, |x25519_s2n_bignum_alt_capable| returns 1 if we detect support
// for bmi+adx instruction sets. Return 0 otherwise.
//
// For all other architectures, return 0.
OPENSSL_INLINE int x25519_s2n_bignum_no_alt_capable(void) {
#if defined(OPENSSL_X86_64)
  if (CRYPTO_is_BMI2_capable() == 1 && CRYPTO_is_ADX_capable() == 1) {
    return 1;
  } else {
    return 0;
  }
#elif defined(OPENSSL_AARCH64)
  return 1;
#else
  return 0;
#endif
}


// Below is the decision logic for which assembly backend implementation
// of x25519 s2n-bignum we should use if x25519 s2n-bignum capable. Currently,
// we support the following implementations.
//
// x86_64:
//   - s2n-bignum-no-alt: hardware implementation using bmi2+adx instruction sets
//   - s2n-bignum-alt: hardware implementation using standard instructions
//
// aarch64:
//   - s2n-bignum-no-alt: hardware implementation for "low" multiplier throughput
//   - s2n-bignum-alt: hardware implementation for "high" multiplier throughput
//
// Through experiments we have found that:
//
// For x86_64: bmi+adc will almost always give a performance boost. So, here we
//   prefer s2n-bignum-no-alt over s2n-bignum-alt if the former is supported.
// For aarch64: if a wide multiplier is supported, we prefer s2n-bignum-alt over
//   s2n-bignum-no-alt if the former is supported.
//   x25519_s2n_bignum_alt_capable() specifically looks to match CPUs that have
//   wide multipliers. this ensures that s2n-bignum-alt will only be used on
//   such CPUs.

static void x25519_s2n_bignum(uint8_t out_shared_key[32],
  const uint8_t private_key[32], const uint8_t peer_public_value[32]) {

  uint8_t private_key_internal_demask[32];
  OPENSSL_memcpy(private_key_internal_demask, private_key, 32);
  private_key_internal_demask[0] &= 248;
  private_key_internal_demask[31] &= 127;
  private_key_internal_demask[31] |= 64;

#if defined(OPENSSL_X86_64)

  if (x25519_s2n_bignum_no_alt_capable() == 1) {
    curve25519_x25519_byte(out_shared_key, private_key_internal_demask,
      peer_public_value);
  } else if (x25519_s2n_bignum_alt_capable() == 1) {
    curve25519_x25519_byte_alt(out_shared_key, private_key_internal_demask,
      peer_public_value);
  } else {
    abort();
  }

#elif defined(OPENSSL_AARCH64)

  if (x25519_s2n_bignum_alt_capable() == 1) {
    curve25519_x25519_byte_alt(out_shared_key, private_key_internal_demask,
      peer_public_value);
  } else if (x25519_s2n_bignum_no_alt_capable() == 1) {
    curve25519_x25519_byte(out_shared_key, private_key_internal_demask,
      peer_public_value);
  } else {
    abort();
  }

#else

  // Should not call this function unless s2n-bignum is supported.
  abort();

#endif
}

static void x25519_s2n_bignum_public_from_private(
  uint8_t out_public_value[32], const uint8_t private_key[32]) {

  uint8_t private_key_internal_demask[32];
  OPENSSL_memcpy(private_key_internal_demask, private_key, 32);
  private_key_internal_demask[0] &= 248;
  private_key_internal_demask[31] &= 127;
  private_key_internal_demask[31] |= 64;

#if defined(OPENSSL_X86_64)

  if (x25519_s2n_bignum_no_alt_capable() == 1) {
    curve25519_x25519base_byte(out_public_value, private_key_internal_demask);
  } else if (x25519_s2n_bignum_alt_capable() == 1) {
    curve25519_x25519base_byte_alt(out_public_value, private_key_internal_demask);
  } else {
    abort();
  }

#elif defined(OPENSSL_AARCH64)

  if (x25519_s2n_bignum_alt_capable() == 1) {
    curve25519_x25519base_byte_alt(out_public_value, private_key_internal_demask);
  } else if (x25519_s2n_bignum_no_alt_capable() == 1) {
    curve25519_x25519base_byte(out_public_value, private_key_internal_demask);
  } else {
    abort();
  }

#else

  // Should not call this function unless s2n-bignum is supported.
  abort();

#endif
}


void ED25519_keypair_from_seed(uint8_t out_public_key[32],
                               uint8_t out_private_key[64],
                               const uint8_t seed[ED25519_SEED_LEN]) {
  uint8_t az[SHA512_DIGEST_LENGTH];
  SHA512(seed, ED25519_SEED_LEN, az);

  az[0] &= 248;
  az[31] &= 127;
  az[31] |= 64;

  ge_p3 A;
  x25519_ge_scalarmult_base(&A, az);
  ge_p3_tobytes(out_public_key, &A);

  OPENSSL_memcpy(out_private_key, seed, ED25519_SEED_LEN);
  OPENSSL_memcpy(out_private_key + ED25519_SEED_LEN, out_public_key, 32);
}

void ED25519_keypair(uint8_t out_public_key[32], uint8_t out_private_key[64]) {
  uint8_t seed[ED25519_SEED_LEN];
  RAND_bytes(seed, ED25519_SEED_LEN);
  ED25519_keypair_from_seed(out_public_key, out_private_key, seed);
  OPENSSL_cleanse(seed, ED25519_SEED_LEN);
}

int ED25519_sign(uint8_t out_sig[64], const uint8_t *message,
                 size_t message_len, const uint8_t private_key[64]) {
  // NOTE: The documentation on this function says that it returns zero on
  // allocation failure. While that can't happen with the current
  // implementation, we want to reserve the ability to allocate in this
  // implementation in the future.

  uint8_t az[SHA512_DIGEST_LENGTH];
  SHA512(private_key, 32, az);

  az[0] &= 248;
  az[31] &= 63;
  az[31] |= 64;

  SHA512_CTX hash_ctx;
  SHA512_Init(&hash_ctx);
  SHA512_Update(&hash_ctx, az + 32, 32);
  SHA512_Update(&hash_ctx, message, message_len);
  uint8_t nonce[SHA512_DIGEST_LENGTH];
  SHA512_Final(nonce, &hash_ctx);

  x25519_sc_reduce(nonce);
  ge_p3 R;
  x25519_ge_scalarmult_base(&R, nonce);
  ge_p3_tobytes(out_sig, &R);

  SHA512_Init(&hash_ctx);
  SHA512_Update(&hash_ctx, out_sig, 32);
  SHA512_Update(&hash_ctx, private_key + 32, 32);
  SHA512_Update(&hash_ctx, message, message_len);
  uint8_t hram[SHA512_DIGEST_LENGTH];
  SHA512_Final(hram, &hash_ctx);

  x25519_sc_reduce(hram);
  sc_muladd(out_sig + 32, hram, az, nonce);

  return 1;
}

int ED25519_verify(const uint8_t *message, size_t message_len,
                   const uint8_t signature[64], const uint8_t public_key[32]) {
  ge_p3 A;
  if ((signature[63] & 224) != 0 ||
      !x25519_ge_frombytes_vartime(&A, public_key)) {
    return 0;
  }

  fe_loose t;
  fe_neg(&t, &A.X);
  fe_carry(&A.X, &t);
  fe_neg(&t, &A.T);
  fe_carry(&A.T, &t);

  uint8_t pkcopy[32];
  OPENSSL_memcpy(pkcopy, public_key, 32);
  uint8_t rcopy[32];
  OPENSSL_memcpy(rcopy, signature, 32);
  uint8_t scopy[32];
  OPENSSL_memcpy(scopy, signature + 32, 32);

  // https://tools.ietf.org/html/rfc8032#section-5.1.7 requires that s be in
  // the range [0, order) in order to prevent signature malleability.

  // kOrder is the order of Curve25519 in little-endian form.
  static const uint64_t kOrder[4] = {
    UINT64_C(0x5812631a5cf5d3ed),
    UINT64_C(0x14def9dea2f79cd6),
    0,
    UINT64_C(0x1000000000000000),
  };
  for (size_t i = 3;; i--) {
    uint64_t word = CRYPTO_load_u64_le(scopy + i * 8);
    if (word > kOrder[i]) {
      return 0;
    } else if (word < kOrder[i]) {
      break;
    } else if (i == 0) {
      return 0;
    }
  }

  SHA512_CTX hash_ctx;
  SHA512_Init(&hash_ctx);
  SHA512_Update(&hash_ctx, signature, 32);
  SHA512_Update(&hash_ctx, public_key, 32);
  SHA512_Update(&hash_ctx, message, message_len);
  uint8_t h[SHA512_DIGEST_LENGTH];
  SHA512_Final(h, &hash_ctx);

  x25519_sc_reduce(h);

  ge_p2 R;
  ge_double_scalarmult_vartime(&R, h, &A, scopy);

  uint8_t rcheck[32];
  x25519_ge_tobytes(rcheck, &R);

  return CRYPTO_memcmp(rcheck, rcopy, sizeof(rcheck)) == 0;
}


void X25519_public_from_private(uint8_t out_public_value[32],
                                const uint8_t private_key[32]) {

  if (x25519_s2n_bignum_capable() == 1) {
    x25519_s2n_bignum_public_from_private(out_public_value, private_key);
  } else {
    x25519_public_from_private_nohw(out_public_value, private_key);
  }
}

void X25519_keypair(uint8_t out_public_value[32], uint8_t out_private_key[32]) {
  RAND_bytes(out_private_key, 32);

  // All X25519 implementations should decode scalars correctly (see
  // https://tools.ietf.org/html/rfc7748#section-5). However, if an
  // implementation doesn't then it might interoperate with random keys a
  // fraction of the time because they'll, randomly, happen to be correctly
  // formed.
  //
  // Thus we do the opposite of the masking here to make sure that our private
  // keys are never correctly masked and so, hopefully, any incorrect
  // implementations are deterministically broken.
  //
  // This does not affect security because, although we're throwing away
  // entropy, a valid implementation of scalarmult should throw away the exact
  // same bits anyway.
  out_private_key[0] |= ~248;
  out_private_key[31] &= ~64;
  out_private_key[31] |= ~127;

  X25519_public_from_private(out_public_value, out_private_key);
}

int X25519(uint8_t out_shared_key[32], const uint8_t private_key[32],
           const uint8_t peer_public_value[32]) {

  static const uint8_t kZeros[32] = {0};

  if (x25519_s2n_bignum_capable() == 1) {
    x25519_s2n_bignum(out_shared_key, private_key, peer_public_value);
  } else {
    x25519_scalar_mult_generic_nohw(out_shared_key, private_key, peer_public_value);
  }

  // The all-zero output results when the input is a point of small order.
  // See https://www.rfc-editor.org/rfc/rfc7748#section-6.1.
  return CRYPTO_memcmp(kZeros, out_shared_key, 32) != 0;
}
