/***********************************************************************
 * Copyright (c) 2014 Pieter Wuille                                    *
 * Distributed under the MIT software license, see the accompanying    *
 * file COPYING or https://www.opensource.org/licenses/mit-license.php.*
 ***********************************************************************/

#ifndef SECP256K1_HASH_H
#define SECP256K1_HASH_H

#include <stdlib.h>
#include <stdint.h>

/* btclock_v4 local patch: on the IDF target (ESP_PLATFORM defined),
 * route secp256k1's SHA-256 through mbedtls so the ESP32-S3 hardware
 * SHA peripheral does the work (CONFIG_MBEDTLS_HARDWARE_SHA=y). Saves
 * ~9 KiB code. The HMAC / RFC6979 layers on top of these functions
 * still call them via the static API and don't need to change.
 *
 * On the host (test_host doctest build, no IDF / no mbedtls), keep
 * the original upstream software SHA-256 implementation. The two
 * paths share the same secp256k1_sha256_{initialize,write,finalize,
 * clear} signatures, just different storage.
 *
 * In mbedtls 3.6 (ESP-IDF v6.0) the public sha256.h was moved under
 * .../mbedtls/private/. Function declarations (MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS)
 * and field access (MBEDTLS_ALLOW_PRIVATE_ACCESS) are opt-in. IDF's
 * own HW SHA port and several IDF components access mbedtls_sha256_*
 * this way; the alternative public mbedtls/md.h dispatcher mallocs
 * an inner ctx in setup() which we don't want on a hot hashing path.
 * If mbedtls eventually removes this path entirely (slated for some
 * future 4.x minor) we'll switch to PSA Crypto's psa_hash_*. */
#if defined(ESP_PLATFORM)
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#define MBEDTLS_ALLOW_PRIVATE_ACCESS
#include "mbedtls/private/sha256.h"

typedef struct {
    mbedtls_sha256_context md;
} secp256k1_sha256;
#else
typedef struct {
    uint32_t s[8];
    unsigned char buf[64];
    uint64_t bytes;
} secp256k1_sha256;
#endif

static void secp256k1_sha256_initialize(secp256k1_sha256 *hash);
static void secp256k1_sha256_write(secp256k1_sha256 *hash, const unsigned char *data, size_t size);
static void secp256k1_sha256_finalize(secp256k1_sha256 *hash, unsigned char *out32);
static void secp256k1_sha256_clear(secp256k1_sha256 *hash);

typedef struct {
    secp256k1_sha256 inner, outer;
} secp256k1_hmac_sha256;

static void secp256k1_hmac_sha256_initialize(secp256k1_hmac_sha256 *hash, const unsigned char *key, size_t size);
static void secp256k1_hmac_sha256_write(secp256k1_hmac_sha256 *hash, const unsigned char *data, size_t size);
static void secp256k1_hmac_sha256_finalize(secp256k1_hmac_sha256 *hash, unsigned char *out32);
static void secp256k1_hmac_sha256_clear(secp256k1_hmac_sha256 *hash);

typedef struct {
    unsigned char v[32];
    unsigned char k[32];
    int retry;
} secp256k1_rfc6979_hmac_sha256;

static void secp256k1_rfc6979_hmac_sha256_initialize(secp256k1_rfc6979_hmac_sha256 *rng, const unsigned char *key, size_t keylen);
static void secp256k1_rfc6979_hmac_sha256_generate(secp256k1_rfc6979_hmac_sha256 *rng, unsigned char *out, size_t outlen);
static void secp256k1_rfc6979_hmac_sha256_finalize(secp256k1_rfc6979_hmac_sha256 *rng);
static void secp256k1_rfc6979_hmac_sha256_clear(secp256k1_rfc6979_hmac_sha256 *rng);

#endif /* SECP256K1_HASH_H */
