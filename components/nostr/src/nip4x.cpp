// NIP-04 + NIP-44 v2 encrypt/decrypt — see nostr/nip4x.hpp for the
// design rationale and host-test-parity argument.
//
// File layout (top to bottom):
//   1. Self-contained SHA-256 (same RFC 6234 textbook code that
//      event_verify.cpp uses — duplicated to keep nip4x.cpp
//      independently auditable; the cost is ~150 LOC vs a shared
//      header).
//   2. HMAC-SHA256 + HKDF Extract/Expand (RFC 2104, RFC 5869).
//   3. ChaCha20 (RFC 8439) — counter starts at 0 per NIP-44 §3.
//   4. AES-256-CBC + PKCS#7 padding (FIPS 197 / RFC 5652 §6.3).
//   5. Base64 (RFC 4648 with padding).
//   6. Constant-time byte equality.
//   7. ECDH via libsecp256k1's `secp256k1_ec_pubkey_tweak_mul` — the
//      raw X-coord variant the spec mandates (NIP-44 §3 "Details" —
//      "in libsecp256k1, unhashed version is available in
//      secp256k1_ec_pubkey_tweak_mul"). We never sign here; the
//      static verify-only context is sufficient.
//   8. NIP-44 v2 padding helpers.
//   9. Public API.

#include "nostr/nip4x.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "secp256k1.h"

// btclock_v4 local: on the IDF target route the CBC layer through
// mbedtls so the ESP32-S3 AES peripheral (CONFIG_MBEDTLS_HARDWARE_AES=y
// across all variants) does the round work. The padding helpers and
// the upstream textbook software AES (gated below) stay so the
// test_host nip4x vectors keep running on the host build, which has
// no IDF / no mbedtls.
//
// mbedtls 4 (IDF v6.0) moved aes.h under mbedtls/private/ and gates
// the function declarations behind MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS.
// We don't poke struct internals — just call the public Encrypt/Decrypt
// API — so MBEDTLS_ALLOW_PRIVATE_ACCESS isn't needed here, unlike the
// secp256k1 SHA wrapper. Same opt-in path IDF's own AES port uses.
#if defined(ESP_PLATFORM)
#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS
#include "mbedtls/private/aes.h"
#endif

namespace btclock {
namespace nostr {
namespace {

// =============================================================== 1. SHA-256

struct Sha256Ctx {
  uint32_t state[8];
  uint64_t bitlen;
  uint8_t buf[64];
  size_t buflen;
};

constexpr uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

constexpr uint32_t Rotr(uint32_t x, unsigned n) {
  return (x >> n) | (x << (32u - n));
}

void Sha256Block(Sha256Ctx& c, const uint8_t* p) {
  uint32_t w[64];
  for (size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<uint32_t>(p[4 * i]) << 24) |
           (static_cast<uint32_t>(p[4 * i + 1]) << 16) |
           (static_cast<uint32_t>(p[4 * i + 2]) << 8) |
           static_cast<uint32_t>(p[4 * i + 3]);
  }
  for (size_t i = 16; i < 64; ++i) {
    const uint32_t s0 =
        Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 =
        Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = c.state[0], b = c.state[1], cc = c.state[2], d = c.state[3];
  uint32_t e = c.state[4], f = c.state[5], g = c.state[6], h = c.state[7];
  for (size_t i = 0; i < 64; ++i) {
    const uint32_t S1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t t1 = h + S1 + ch + kSha256K[i] + w[i];
    const uint32_t S0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
    const uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
    const uint32_t t2 = S0 + mj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = cc;
    cc = b;
    b = a;
    a = t1 + t2;
  }
  c.state[0] += a;
  c.state[1] += b;
  c.state[2] += cc;
  c.state[3] += d;
  c.state[4] += e;
  c.state[5] += f;
  c.state[6] += g;
  c.state[7] += h;
}

void Sha256Init(Sha256Ctx& c) {
  c.state[0] = 0x6a09e667u;
  c.state[1] = 0xbb67ae85u;
  c.state[2] = 0x3c6ef372u;
  c.state[3] = 0xa54ff53au;
  c.state[4] = 0x510e527fu;
  c.state[5] = 0x9b05688cu;
  c.state[6] = 0x1f83d9abu;
  c.state[7] = 0x5be0cd19u;
  c.bitlen = 0;
  c.buflen = 0;
}

void Sha256Update(Sha256Ctx& c, const uint8_t* data, size_t n) {
  c.bitlen += static_cast<uint64_t>(n) * 8u;
  while (n != 0) {
    const size_t take = (64 - c.buflen) < n ? (64 - c.buflen) : n;
    std::memcpy(c.buf + c.buflen, data, take);
    c.buflen += take;
    data += take;
    n -= take;
    if (c.buflen == 64) {
      Sha256Block(c, c.buf);
      c.buflen = 0;
    }
  }
}

void Sha256Final(Sha256Ctx& c, uint8_t out[32]) {
  c.buf[c.buflen++] = 0x80;
  if (c.buflen > 56) {
    while (c.buflen < 64) c.buf[c.buflen++] = 0;
    Sha256Block(c, c.buf);
    c.buflen = 0;
  }
  while (c.buflen < 56) c.buf[c.buflen++] = 0;
  for (int i = 7; i >= 0; --i) {
    c.buf[c.buflen++] = static_cast<uint8_t>(
        (c.bitlen >> (static_cast<unsigned>(i) * 8u)) & 0xffu);
  }
  Sha256Block(c, c.buf);
  for (size_t i = 0; i < 8; ++i) {
    out[4 * i] = static_cast<uint8_t>(c.state[i] >> 24);
    out[4 * i + 1] = static_cast<uint8_t>(c.state[i] >> 16);
    out[4 * i + 2] = static_cast<uint8_t>(c.state[i] >> 8);
    out[4 * i + 3] = static_cast<uint8_t>(c.state[i]);
  }
}

void Sha256(const uint8_t* data, size_t n, uint8_t out[32]) {
  Sha256Ctx c;
  Sha256Init(c);
  Sha256Update(c, data, n);
  Sha256Final(c, out);
}

// =================================================== 2. HMAC-SHA256 + HKDF

void HmacSha256(const uint8_t* key, size_t key_len, const uint8_t* msg,
                size_t msg_len, uint8_t out[32]) {
  // Block size of SHA-256 is 64 bytes (key normalization spec, RFC 2104 §2).
  uint8_t k_block[64];
  std::memset(k_block, 0, sizeof(k_block));
  if (key_len > 64) {
    Sha256(key, key_len, k_block);
    // Remaining 32 bytes already zero.
  } else {
    std::memcpy(k_block, key, key_len);
  }

  uint8_t ipad[64];
  uint8_t opad[64];
  for (size_t i = 0; i < 64; ++i) {
    ipad[i] = static_cast<uint8_t>(k_block[i] ^ 0x36u);
    opad[i] = static_cast<uint8_t>(k_block[i] ^ 0x5cu);
  }

  uint8_t inner[32];
  {
    Sha256Ctx c;
    Sha256Init(c);
    Sha256Update(c, ipad, 64);
    Sha256Update(c, msg, msg_len);
    Sha256Final(c, inner);
  }
  {
    Sha256Ctx c;
    Sha256Init(c);
    Sha256Update(c, opad, 64);
    Sha256Update(c, inner, 32);
    Sha256Final(c, out);
  }
}

// HKDF-Extract(salt, IKM) = HMAC-SHA256(salt, IKM). RFC 5869 §2.2.
void HkdfExtract(const uint8_t* salt, size_t salt_len, const uint8_t* ikm,
                 size_t ikm_len, uint8_t prk[32]) {
  HmacSha256(salt, salt_len, ikm, ikm_len, prk);
}

// HKDF-Expand(PRK, info, L). RFC 5869 §2.3. Returns false if L is out
// of range (> 255 * HashLen). NIP-44 v2 only ever asks for L=76 so
// the loop in practice runs at most 3 iterations (ceil(76/32) = 3).
bool HkdfExpand(const uint8_t prk[32], const uint8_t* info, size_t info_len,
                uint8_t* out, size_t out_len) {
  if (out_len == 0) return true;
  if (out_len > 255 * 32) return false;
  const size_t n = (out_len + 31) / 32;
  uint8_t t[32];
  size_t produced = 0;
  for (size_t i = 1; i <= n; ++i) {
    // T(i) = HMAC(PRK, T(i-1) || info || octet(i))
    uint8_t buf[32 + 1];
    size_t buf_len = 0;
    if (i > 1) {
      std::memcpy(buf, t, 32);
      buf_len = 32;
    }
    Sha256Ctx unused;
    (void)unused;
    // Compose the HMAC message in-place. We could call HmacSha256
    // with separate buffers but the prefix-then-suffix shape costs an
    // extra alloc; reuse two HMAC calls with manual concatenation
    // would be faster but the wrapper above is the simplest correct
    // path and runs in microseconds at L=76.
    {
      // Use a small scratch vector to avoid std::vector for tiny info.
      const size_t total = buf_len + info_len + 1;
      // Stack-allocate for the typical (32-byte info) case; fall back
      // to heap for paranoia.
      uint8_t stack[32 + 64 + 1];
      uint8_t* msg = (total <= sizeof(stack)) ? stack : new uint8_t[total];
      std::memcpy(msg, buf, buf_len);
      if (info_len != 0) std::memcpy(msg + buf_len, info, info_len);
      msg[buf_len + info_len] = static_cast<uint8_t>(i);
      HmacSha256(prk, 32, msg, total, t);
      if (msg != stack) delete[] msg;
    }
    const size_t take = (produced + 32 <= out_len) ? 32 : (out_len - produced);
    std::memcpy(out + produced, t, take);
    produced += take;
  }
  return true;
}

// ====================================================== 3. ChaCha20 (RFC 8439)

constexpr uint32_t kChaChaConst[4] = {0x61707865u, 0x3320646eu, 0x79622d32u,
                                      0x6b206574u};

inline uint32_t ChaChaRotl(uint32_t x, unsigned n) {
  return (x << n) | (x >> (32u - n));
}

inline void ChaChaQuarterRound(uint32_t& a, uint32_t& b, uint32_t& c,
                               uint32_t& d) {
  a += b;
  d ^= a;
  d = ChaChaRotl(d, 16);
  c += d;
  b ^= c;
  b = ChaChaRotl(b, 12);
  a += b;
  d ^= a;
  d = ChaChaRotl(d, 8);
  c += d;
  b ^= c;
  b = ChaChaRotl(b, 7);
}

void ChaChaBlock(const uint32_t state[16], uint8_t out[64]) {
  uint32_t s[16];
  std::memcpy(s, state, sizeof(s));
  for (int i = 0; i < 10; ++i) {
    // Two interleaved rounds per outer iteration → 20 rounds total.
    ChaChaQuarterRound(s[0], s[4], s[8], s[12]);
    ChaChaQuarterRound(s[1], s[5], s[9], s[13]);
    ChaChaQuarterRound(s[2], s[6], s[10], s[14]);
    ChaChaQuarterRound(s[3], s[7], s[11], s[15]);
    ChaChaQuarterRound(s[0], s[5], s[10], s[15]);
    ChaChaQuarterRound(s[1], s[6], s[11], s[12]);
    ChaChaQuarterRound(s[2], s[7], s[8], s[13]);
    ChaChaQuarterRound(s[3], s[4], s[9], s[14]);
  }
  for (int i = 0; i < 16; ++i) {
    const uint32_t v = s[i] + state[i];
    out[4 * i] = static_cast<uint8_t>(v);
    out[4 * i + 1] = static_cast<uint8_t>(v >> 8);
    out[4 * i + 2] = static_cast<uint8_t>(v >> 16);
    out[4 * i + 3] = static_cast<uint8_t>(v >> 24);
  }
}

inline uint32_t ChaChaLoad32Le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

// ChaCha20 with starting counter = 0 (NIP-44 v2 spec — RFC 8439 §2.4
// uses starting counter 1 for AEAD; NIP-44 explicitly opts for 0).
void ChaCha20(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* in,
              uint8_t* out, size_t len) {
  uint32_t state[16];
  state[0] = kChaChaConst[0];
  state[1] = kChaChaConst[1];
  state[2] = kChaChaConst[2];
  state[3] = kChaChaConst[3];
  for (int i = 0; i < 8; ++i) state[4 + i] = ChaChaLoad32Le(key + 4 * i);
  state[12] = 0;
  for (int i = 0; i < 3; ++i) state[13 + i] = ChaChaLoad32Le(nonce + 4 * i);

  uint8_t block[64];
  while (len > 0) {
    ChaChaBlock(state, block);
    const size_t take = len < 64 ? len : 64;
    for (size_t i = 0; i < take; ++i) {
      out[i] = static_cast<uint8_t>(in[i] ^ block[i]);
    }
    in += take;
    out += take;
    len -= take;
    state[12] += 1;
    // 32-bit counter wrap would mean we're encrypting >256 GiB with a
    // single nonce — unreachable at our payload sizes. No carry into
    // the nonce.
  }
}

// =========================================================== 4. AES-256-CBC
//
// On ESP_PLATFORM the round work goes through mbedtls_aes_crypt_cbc;
// the textbook AES below is compiled only on the host build. PKCS#7
// padding/strip lives in the Aes256CbcPkcs7* wrappers — mbedtls's AES
// API takes already-padded buffers, and mbedtls_cipher_* (which does
// padding) mallocs an inner ctx every call.
#if !defined(ESP_PLATFORM)

// AES S-box (FIPS 197 §5.1.1).
constexpr uint8_t kAesSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b,
    0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
    0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26,
    0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
    0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed,
    0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f,
    0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec,
    0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14,
    0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
    0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f,
    0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
    0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f,
    0xb0, 0x54, 0xbb, 0x16};

// Inverse S-box (FIPS 197 §5.3.2).
constexpr uint8_t kAesInvSbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e,
    0x81, 0xf3, 0xd7, 0xfb, 0x7c, 0xe3, 0x39, 0x82, 0x9b, 0x2f, 0xff, 0x87,
    0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb, 0x54, 0x7b, 0x94, 0x32,
    0xa6, 0xc2, 0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e,
    0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76, 0x5b, 0xa2, 0x49,
    0x6d, 0x8b, 0xd1, 0x25, 0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16,
    0xd4, 0xa4, 0x5c, 0xcc, 0x5d, 0x65, 0xb6, 0x92, 0x6c, 0x70, 0x48, 0x50,
    0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d, 0x84,
    0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05,
    0xb8, 0xb3, 0x45, 0x06, 0xd0, 0x2c, 0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02,
    0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b, 0x3a, 0x91, 0x11, 0x41,
    0x4f, 0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73,
    0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85, 0xe2, 0xf9, 0x37, 0xe8,
    0x1c, 0x75, 0xdf, 0x6e, 0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89,
    0x6f, 0xb7, 0x62, 0x0e, 0xaa, 0x18, 0xbe, 0x1b, 0xfc, 0x56, 0x3e, 0x4b,
    0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd, 0x5a, 0xf4,
    0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59,
    0x27, 0x80, 0xec, 0x5f, 0x60, 0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d,
    0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef, 0xa0, 0xe0, 0x3b, 0x4d,
    0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6, 0x26, 0xe1, 0x69, 0x14, 0x63,
    0x55, 0x21, 0x0c, 0x7d};

// Round constants. FIPS 197 §5.2 — Rcon[i] starts at i=1 with x^(i-1)
// in GF(2^8). AES-256 uses 7 round constants (Nk=8, Nr=14, key schedule
// invokes Rcon at every Nk-th word: i = 8, 16, 24, 32, 40, 48, 56).
constexpr uint8_t kAesRcon[8] = {0x00, 0x01, 0x02, 0x04,
                                 0x08, 0x10, 0x20, 0x40};

// Xtime — multiply by x in GF(2^8) with reduction polynomial 0x11b.
constexpr uint8_t Xtime(uint8_t x) {
  const unsigned shifted = static_cast<unsigned>(x) << 1;
  const unsigned reduce = (x & 0x80u) ? 0x1bu : 0x00u;
  return static_cast<uint8_t>(shifted ^ reduce);
}

void AesKeyExpansion(const uint8_t key[32], uint8_t round_keys[240]) {
  // 60 32-bit words = 240 bytes; AES-256 (Nk=8, Nr=14) produces
  // Nb*(Nr+1) = 4*15 = 60 words.
  std::memcpy(round_keys, key, 32);
  uint8_t temp[4];
  for (size_t i = 32; i < 240; i += 4) {
    temp[0] = round_keys[i - 4];
    temp[1] = round_keys[i - 3];
    temp[2] = round_keys[i - 2];
    temp[3] = round_keys[i - 1];
    if ((i % 32) == 0) {
      // RotWord + SubWord + Rcon.
      const uint8_t t0 = temp[0];
      temp[0] = static_cast<uint8_t>(kAesSbox[temp[1]] ^ kAesRcon[i / 32]);
      temp[1] = kAesSbox[temp[2]];
      temp[2] = kAesSbox[temp[3]];
      temp[3] = kAesSbox[t0];
    } else if ((i % 32) == 16) {
      // Extra SubWord every 8 words for AES-256 (FIPS 197 §5.2).
      temp[0] = kAesSbox[temp[0]];
      temp[1] = kAesSbox[temp[1]];
      temp[2] = kAesSbox[temp[2]];
      temp[3] = kAesSbox[temp[3]];
    }
    round_keys[i] = static_cast<uint8_t>(round_keys[i - 32] ^ temp[0]);
    round_keys[i + 1] = static_cast<uint8_t>(round_keys[i - 31] ^ temp[1]);
    round_keys[i + 2] = static_cast<uint8_t>(round_keys[i - 30] ^ temp[2]);
    round_keys[i + 3] = static_cast<uint8_t>(round_keys[i - 29] ^ temp[3]);
  }
}

void AesAddRoundKey(uint8_t state[16], const uint8_t* rk) {
  for (int i = 0; i < 16; ++i)
    state[i] = static_cast<uint8_t>(state[i] ^ rk[i]);
}

void AesSubBytes(uint8_t state[16]) {
  for (int i = 0; i < 16; ++i) state[i] = kAesSbox[state[i]];
}

void AesInvSubBytes(uint8_t state[16]) {
  for (int i = 0; i < 16; ++i) state[i] = kAesInvSbox[state[i]];
}

void AesShiftRows(uint8_t s[16]) {
  // Row 1: shift left by 1.
  uint8_t t = s[1];
  s[1] = s[5];
  s[5] = s[9];
  s[9] = s[13];
  s[13] = t;
  // Row 2: shift left by 2.
  t = s[2];
  s[2] = s[10];
  s[10] = t;
  t = s[6];
  s[6] = s[14];
  s[14] = t;
  // Row 3: shift left by 3.
  t = s[3];
  s[3] = s[15];
  s[15] = s[11];
  s[11] = s[7];
  s[7] = t;
}

void AesInvShiftRows(uint8_t s[16]) {
  // Inverse of ShiftRows.
  uint8_t t = s[13];
  s[13] = s[9];
  s[9] = s[5];
  s[5] = s[1];
  s[1] = t;
  t = s[2];
  s[2] = s[10];
  s[10] = t;
  t = s[6];
  s[6] = s[14];
  s[14] = t;
  t = s[3];
  s[3] = s[7];
  s[7] = s[11];
  s[11] = s[15];
  s[15] = t;
}

void AesMixColumns(uint8_t s[16]) {
  for (int c = 0; c < 4; ++c) {
    const uint8_t a0 = s[4 * c];
    const uint8_t a1 = s[4 * c + 1];
    const uint8_t a2 = s[4 * c + 2];
    const uint8_t a3 = s[4 * c + 3];
    const uint8_t x = static_cast<uint8_t>(a0 ^ a1 ^ a2 ^ a3);
    s[4 * c] = static_cast<uint8_t>(s[4 * c] ^ x ^ Xtime(a0 ^ a1));
    s[4 * c + 1] = static_cast<uint8_t>(s[4 * c + 1] ^ x ^ Xtime(a1 ^ a2));
    s[4 * c + 2] = static_cast<uint8_t>(s[4 * c + 2] ^ x ^ Xtime(a2 ^ a3));
    s[4 * c + 3] = static_cast<uint8_t>(s[4 * c + 3] ^ x ^ Xtime(a3 ^ a0));
  }
}

inline uint8_t AesGmul(uint8_t a, uint8_t b) {
  uint8_t r = 0;
  for (int i = 0; i < 8; ++i) {
    if (b & 1) r = static_cast<uint8_t>(r ^ a);
    a = Xtime(a);
    b = static_cast<uint8_t>(b >> 1);
  }
  return r;
}

void AesInvMixColumns(uint8_t s[16]) {
  for (int c = 0; c < 4; ++c) {
    const uint8_t a0 = s[4 * c];
    const uint8_t a1 = s[4 * c + 1];
    const uint8_t a2 = s[4 * c + 2];
    const uint8_t a3 = s[4 * c + 3];
    s[4 * c] = static_cast<uint8_t>(AesGmul(a0, 0x0e) ^ AesGmul(a1, 0x0b) ^
                                    AesGmul(a2, 0x0d) ^ AesGmul(a3, 0x09));
    s[4 * c + 1] = static_cast<uint8_t>(AesGmul(a0, 0x09) ^ AesGmul(a1, 0x0e) ^
                                        AesGmul(a2, 0x0b) ^ AesGmul(a3, 0x0d));
    s[4 * c + 2] = static_cast<uint8_t>(AesGmul(a0, 0x0d) ^ AesGmul(a1, 0x09) ^
                                        AesGmul(a2, 0x0e) ^ AesGmul(a3, 0x0b));
    s[4 * c + 3] = static_cast<uint8_t>(AesGmul(a0, 0x0b) ^ AesGmul(a1, 0x0d) ^
                                        AesGmul(a2, 0x09) ^ AesGmul(a3, 0x0e));
  }
}

void Aes256EncryptBlock(const uint8_t* round_keys, const uint8_t in[16],
                        uint8_t out[16]) {
  uint8_t s[16];
  std::memcpy(s, in, 16);
  AesAddRoundKey(s, round_keys);
  for (int r = 1; r < 14; ++r) {
    AesSubBytes(s);
    AesShiftRows(s);
    AesMixColumns(s);
    AesAddRoundKey(s, round_keys + 16 * r);
  }
  AesSubBytes(s);
  AesShiftRows(s);
  AesAddRoundKey(s, round_keys + 16 * 14);
  std::memcpy(out, s, 16);
}

void Aes256DecryptBlock(const uint8_t* round_keys, const uint8_t in[16],
                        uint8_t out[16]) {
  uint8_t s[16];
  std::memcpy(s, in, 16);
  AesAddRoundKey(s, round_keys + 16 * 14);
  for (int r = 13; r >= 1; --r) {
    AesInvShiftRows(s);
    AesInvSubBytes(s);
    AesAddRoundKey(s, round_keys + 16 * r);
    AesInvMixColumns(s);
  }
  AesInvShiftRows(s);
  AesInvSubBytes(s);
  AesAddRoundKey(s, round_keys);
  std::memcpy(out, s, 16);
}

#endif  // !defined(ESP_PLATFORM)

// AES-256-CBC with PKCS#7 padding. `out` must have room for
// `in_len + (16 - in_len % 16)` bytes (i.e. always at least one full
// padding block). Returns the ciphertext length.
//
// On ESP_PLATFORM the inner CBC loop runs on the ESP32-S3 AES HW
// peripheral via mbedtls; on host it uses the textbook implementation
// above. Padding is applied in-place into a scratch buffer before the
// single mbedtls_aes_crypt_cbc call because mbedtls's plain AES API
// only handles already-padded input (the padded-mode cipher layer
// mallocs an inner ctx per call, which we don't want here).
size_t Aes256CbcPkcs7Encrypt(const uint8_t key[32], const uint8_t iv[16],
                             const uint8_t* in, size_t in_len, uint8_t* out) {
  const size_t pad = 16 - (in_len % 16);
  const size_t total = in_len + pad;
#if defined(ESP_PLATFORM)
  // Build the padded plaintext directly in `out` (caller-supplied,
  // already sized for `total`), then encrypt in place.
  std::memcpy(out, in, in_len);
  for (size_t i = in_len; i < total; ++i) out[i] = static_cast<uint8_t>(pad);
  uint8_t iv_copy[16];
  std::memcpy(iv_copy, iv, 16);  // mbedtls updates iv in-place; protect caller.
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  (void)mbedtls_aes_setkey_enc(&ctx, key, 256);
  (void)mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, total, iv_copy, out,
                              out);
  mbedtls_aes_free(&ctx);
#else
  uint8_t round_keys[240];
  AesKeyExpansion(key, round_keys);
  uint8_t prev[16];
  std::memcpy(prev, iv, 16);
  uint8_t block[16];
  for (size_t off = 0; off < total; off += 16) {
    const size_t take = (off + 16 <= in_len) ? 16 : (in_len - off);
    std::memcpy(block, in + off, take);
    // PKCS#7 — fill the rest of this block (possibly all of it) with
    // the pad byte. Always at least one byte; if input length is a
    // multiple of 16, this is an entire trailing block of 0x10s.
    for (size_t i = take; i < 16; ++i) block[i] = static_cast<uint8_t>(pad);
    for (int i = 0; i < 16; ++i)
      block[i] = static_cast<uint8_t>(block[i] ^ prev[i]);
    Aes256EncryptBlock(round_keys, block, out + off);
    std::memcpy(prev, out + off, 16);
  }
#endif
  return total;
}

// AES-256-CBC decrypt with PKCS#7 strip. Returns plaintext length on
// success, or SIZE_MAX on malformed padding / wrong-multiple input.
size_t Aes256CbcPkcs7Decrypt(const uint8_t key[32], const uint8_t iv[16],
                             const uint8_t* in, size_t in_len, uint8_t* out) {
  if (in_len == 0 || (in_len % 16) != 0) return SIZE_MAX;
#if defined(ESP_PLATFORM)
  uint8_t iv_copy[16];
  std::memcpy(iv_copy, iv, 16);
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  (void)mbedtls_aes_setkey_dec(&ctx, key, 256);
  (void)mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, in_len, iv_copy, in,
                              out);
  mbedtls_aes_free(&ctx);
#else
  uint8_t round_keys[240];
  AesKeyExpansion(key, round_keys);
  uint8_t prev[16];
  std::memcpy(prev, iv, 16);
  uint8_t block[16];
  for (size_t off = 0; off < in_len; off += 16) {
    Aes256DecryptBlock(round_keys, in + off, block);
    for (size_t i = 0; i < 16; ++i)
      out[off + i] = static_cast<uint8_t>(block[i] ^ prev[i]);
    std::memcpy(prev, in + off, 16);
  }
#endif
  // Strip PKCS#7.
  const uint8_t pad = out[in_len - 1];
  if (pad == 0 || pad > 16) return SIZE_MAX;
  for (size_t i = 0; i < pad; ++i) {
    if (out[in_len - 1 - i] != pad) return SIZE_MAX;
  }
  return in_len - pad;
}

// ====================================================== 5. Base64 (RFC 4648)

constexpr const char* kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64Encode(const uint8_t* data, size_t len) {
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= len) {
    const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                       (static_cast<uint32_t>(data[i + 1]) << 8) |
                       static_cast<uint32_t>(data[i + 2]);
    out.push_back(kBase64Alphabet[(v >> 18) & 0x3fu]);
    out.push_back(kBase64Alphabet[(v >> 12) & 0x3fu]);
    out.push_back(kBase64Alphabet[(v >> 6) & 0x3fu]);
    out.push_back(kBase64Alphabet[v & 0x3fu]);
    i += 3;
  }
  if (i < len) {
    const size_t remain = len - i;
    uint32_t v = static_cast<uint32_t>(data[i]) << 16;
    if (remain == 2) v |= static_cast<uint32_t>(data[i + 1]) << 8;
    out.push_back(kBase64Alphabet[(v >> 18) & 0x3fu]);
    out.push_back(kBase64Alphabet[(v >> 12) & 0x3fu]);
    out.push_back(remain == 2 ? kBase64Alphabet[(v >> 6) & 0x3fu] : '=');
    out.push_back('=');
  }
  return out;
}

bool Base64Decode(const std::string& s, std::string& out) {
  out.clear();
  if (s.empty()) return true;
  if (s.size() % 4 != 0) return false;
  // Reverse-lookup table — 0xff for invalid, value otherwise. Built
  // at first use; static-local thread safety isn't an issue here
  // because the table is read-only after init.
  static uint8_t lut[256];
  static bool lut_ready = false;
  if (!lut_ready) {
    std::memset(lut, 0xff, sizeof(lut));
    for (uint8_t i = 0; i < 64; ++i)
      lut[static_cast<uint8_t>(kBase64Alphabet[i])] = i;
    lut_ready = true;
  }
  out.reserve((s.size() / 4) * 3);
  for (size_t i = 0; i < s.size(); i += 4) {
    const char c0 = s[i];
    const char c1 = s[i + 1];
    const char c2 = s[i + 2];
    const char c3 = s[i + 3];
    const uint8_t v0 = lut[static_cast<uint8_t>(c0)];
    const uint8_t v1 = lut[static_cast<uint8_t>(c1)];
    if (v0 == 0xffu || v1 == 0xffu) return false;
    uint32_t v =
        (static_cast<uint32_t>(v0) << 18) | (static_cast<uint32_t>(v1) << 12);
    if (c2 == '=') {
      // Last quartet, two pad chars — output exactly 1 byte.
      if (c3 != '=' || i + 4 != s.size()) return false;
      out.push_back(static_cast<char>((v >> 16) & 0xffu));
      return true;
    }
    const uint8_t v2 = lut[static_cast<uint8_t>(c2)];
    if (v2 == 0xffu) return false;
    v |= static_cast<uint32_t>(v2) << 6;
    if (c3 == '=') {
      // Last quartet, one pad char — output exactly 2 bytes.
      if (i + 4 != s.size()) return false;
      out.push_back(static_cast<char>((v >> 16) & 0xffu));
      out.push_back(static_cast<char>((v >> 8) & 0xffu));
      return true;
    }
    const uint8_t v3 = lut[static_cast<uint8_t>(c3)];
    if (v3 == 0xffu) return false;
    v |= static_cast<uint32_t>(v3);
    out.push_back(static_cast<char>((v >> 16) & 0xffu));
    out.push_back(static_cast<char>((v >> 8) & 0xffu));
    out.push_back(static_cast<char>(v & 0xffu));
  }
  return true;
}

// =================================================== 6. Constant-time compare

bool ConstantTimeEqualBytes(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t acc = 0;
  for (size_t i = 0; i < n; ++i)
    acc = static_cast<uint8_t>(acc | (a[i] ^ b[i]));
  return acc == 0;
}

// ============================================= 7. ECDH (raw X via
// libsecp256k1)

// Given our 32-byte private key and the peer's 32-byte x-only public
// key (BIP-340 form), compute the 32-byte X coordinate of (sec * pub).
// `pub32` is lifted with even Y per BIP-340 — equivalent to parsing
// the compressed point 0x02 || pub32. Returns false if either key is
// invalid (privkey == 0 or >= curve order, or pubkey x not on curve).
//
// Implementation detail: we go via `secp256k1_ec_pubkey_tweak_mul`
// rather than the (unvendored) ecdh module. Spec note (NIP-44 §3
// "Details"): "in libsecp256k1, unhashed version is available in
// `secp256k1_ec_pubkey_tweak_mul`". Same code path on host and target.
bool EcdhSharedX(const uint8_t seckey32[32], const uint8_t pub32[32],
                 uint8_t shared_x[32]) {
  uint8_t compressed[33];
  compressed[0] = 0x02;  // BIP-340 lift_x picks even Y.
  std::memcpy(compressed + 1, pub32, 32);
  secp256k1_pubkey pk;
  if (secp256k1_ec_pubkey_parse(secp256k1_context_static, &pk, compressed,
                                33) != 1) {
    return false;
  }
  if (secp256k1_ec_pubkey_tweak_mul(secp256k1_context_static, &pk, seckey32) !=
      1) {
    return false;
  }
  uint8_t out[65];
  size_t out_len = sizeof(out);
  if (secp256k1_ec_pubkey_serialize(secp256k1_context_static, out, &out_len,
                                    &pk, SECP256K1_EC_UNCOMPRESSED) != 1) {
    return false;
  }
  // out = 0x04 || X(32) || Y(32). Take the X coordinate.
  std::memcpy(shared_x, out + 1, 32);
  return true;
}

// ====================================================== 8. NIP-44 v2 padding

// Pseudocode from NIP-44 §3 — translated to integer math (no float
// log2; ESP32-S3 has no FPU on the integer pipeline this matters for,
// and we'd be exposed to corner-case rounding either way).
size_t Nip44CalcPaddedLen(size_t unpadded_len) {
  if (unpadded_len <= 32) return 32;
  // next_power = 1 << (floor(log2(n - 1)) + 1)
  size_t n_minus_1 = unpadded_len - 1;
  unsigned shift = 0;
  while (n_minus_1 > 1) {
    n_minus_1 >>= 1;
    ++shift;
  }
  const size_t next_power = static_cast<size_t>(1) << (shift + 1);
  const size_t chunk = (next_power <= 256) ? 32 : (next_power / 8);
  return chunk * (((unpadded_len - 1) / chunk) + 1);
}

}  // namespace

// =========================================================== 9. Public API

bool Nip44ConversationKey(const uint8_t seckey32[32], const uint8_t pub32[32],
                          uint8_t conversation_key32[32]) {
  uint8_t shared_x[32];
  if (!EcdhSharedX(seckey32, pub32, shared_x)) return false;
  // HKDF-Extract uses utf8('nip44-v2') as the salt per NIP-44 §3.
  static const char kSalt[] = "nip44-v2";
  HkdfExtract(reinterpret_cast<const uint8_t*>(kSalt), sizeof(kSalt) - 1,
              shared_x, 32, conversation_key32);
  return true;
}

namespace {

// Derive (chacha_key, chacha_nonce, hmac_key) from a conversation key
// and per-message nonce. Returns by reference into three caller-
// supplied buffers; total HKDF-Expand output is 32+12+32 = 76 bytes.
void Nip44MessageKeys(const uint8_t conversation_key32[32],
                      const uint8_t nonce32[32], uint8_t chacha_key32[32],
                      uint8_t chacha_nonce12[12], uint8_t hmac_key32[32]) {
  uint8_t keys[76];
  HkdfExpand(conversation_key32, nonce32, 32, keys, 76);
  std::memcpy(chacha_key32, keys, 32);
  std::memcpy(chacha_nonce12, keys + 32, 12);
  std::memcpy(hmac_key32, keys + 44, 32);
}

}  // namespace

std::string Nip44EncryptV2(const uint8_t conversation_key32[32],
                           const uint8_t nonce32[32],
                           const std::string& plaintext) {
  const size_t pt_len = plaintext.size();
  if (pt_len < 1 || pt_len > 65535) return std::string();

  const size_t padded_len = Nip44CalcPaddedLen(pt_len);
  // Padded blob: u16 BE length || plaintext || zero pad up to padded_len.
  std::string padded;
  padded.resize(2 + padded_len);
  padded[0] = static_cast<char>((pt_len >> 8) & 0xffu);
  padded[1] = static_cast<char>(pt_len & 0xffu);
  std::memcpy(&padded[2], plaintext.data(), pt_len);
  // Remaining bytes default to zero from resize.

  uint8_t chacha_key[32];
  uint8_t chacha_nonce[12];
  uint8_t hmac_key[32];
  Nip44MessageKeys(conversation_key32, nonce32, chacha_key, chacha_nonce,
                   hmac_key);

  std::string ciphertext;
  ciphertext.resize(padded.size());
  ChaCha20(chacha_key, chacha_nonce,
           reinterpret_cast<const uint8_t*>(padded.data()),
           reinterpret_cast<uint8_t*>(&ciphertext[0]), padded.size());

  // MAC AAD = nonce; HMAC(hmac_key, nonce || ciphertext).
  std::string hmac_input;
  hmac_input.resize(32 + ciphertext.size());
  std::memcpy(&hmac_input[0], nonce32, 32);
  std::memcpy(&hmac_input[32], ciphertext.data(), ciphertext.size());
  uint8_t mac[32];
  HmacSha256(hmac_key, 32, reinterpret_cast<const uint8_t*>(hmac_input.data()),
             hmac_input.size(), mac);

  std::string raw;
  raw.resize(1 + 32 + ciphertext.size() + 32);
  raw[0] = 0x02;
  std::memcpy(&raw[1], nonce32, 32);
  std::memcpy(&raw[33], ciphertext.data(), ciphertext.size());
  std::memcpy(&raw[33 + ciphertext.size()], mac, 32);
  return Base64Encode(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
}

Nip4xDecryptResult Nip44DecryptV2(const uint8_t conversation_key32[32],
                                  const std::string& payload) {
  Nip4xDecryptResult r;

  // Per NIP-44 §3 step 1: a leading '#' is a future-proof flag for
  // alternate encodings; current spec mandates rejecting it with
  // "version not supported".
  if (payload.empty() || payload[0] == '#') {
    r.error = Nip4xError::kUnknownVersion;
    return r;
  }
  // Payload length bounds prevent DoS on the base64 decoder.
  if (payload.size() < 132 || payload.size() > 87472) {
    r.error = Nip4xError::kBadPayload;
    return r;
  }
  std::string raw;
  if (!Base64Decode(payload, raw)) {
    r.error = Nip4xError::kBadPayload;
    return r;
  }
  if (raw.size() < 99 || raw.size() > 65603) {
    r.error = Nip4xError::kBadPayload;
    return r;
  }
  if (static_cast<uint8_t>(raw[0]) != 0x02) {
    r.error = Nip4xError::kUnknownVersion;
    return r;
  }

  const size_t ct_len = raw.size() - 1 - 32 - 32;
  const uint8_t* nonce = reinterpret_cast<const uint8_t*>(raw.data() + 1);
  const uint8_t* ciphertext = reinterpret_cast<const uint8_t*>(raw.data() + 33);
  const uint8_t* mac =
      reinterpret_cast<const uint8_t*>(raw.data() + 33 + ct_len);

  uint8_t chacha_key[32];
  uint8_t chacha_nonce[12];
  uint8_t hmac_key[32];
  Nip44MessageKeys(conversation_key32, nonce, chacha_key, chacha_nonce,
                   hmac_key);

  // Compute MAC over (nonce || ciphertext) and constant-time compare.
  std::string hmac_input;
  hmac_input.resize(32 + ct_len);
  std::memcpy(&hmac_input[0], nonce, 32);
  std::memcpy(&hmac_input[32], ciphertext, ct_len);
  uint8_t recomputed_mac[32];
  HmacSha256(hmac_key, 32, reinterpret_cast<const uint8_t*>(hmac_input.data()),
             hmac_input.size(), recomputed_mac);
  if (!ConstantTimeEqualBytes(mac, recomputed_mac, 32)) {
    r.error = Nip4xError::kMacMismatch;
    return r;
  }

  // ChaCha20-decrypt (XOR with the same keystream).
  std::string padded;
  padded.resize(ct_len);
  ChaCha20(chacha_key, chacha_nonce, ciphertext,
           reinterpret_cast<uint8_t*>(&padded[0]), ct_len);

  // Unpad: read 2-byte BE length prefix and verify the padded buffer
  // size matches what CalcPaddedLen says it should be. Verifying the
  // size catches both length-prefix tampering and a wrong padded
  // chunk size.
  if (padded.size() < 2) {
    r.error = Nip4xError::kBadPadding;
    return r;
  }
  const size_t plaintext_len =
      (static_cast<size_t>(static_cast<uint8_t>(padded[0])) << 8) |
      static_cast<size_t>(static_cast<uint8_t>(padded[1]));
  if (plaintext_len == 0 || plaintext_len > 65535) {
    r.error = Nip4xError::kBadPadding;
    return r;
  }
  if (padded.size() != 2 + Nip44CalcPaddedLen(plaintext_len)) {
    r.error = Nip4xError::kBadPadding;
    return r;
  }
  if (2 + plaintext_len > padded.size()) {
    r.error = Nip4xError::kBadPadding;
    return r;
  }

  r.ok = true;
  r.plaintext.assign(padded.data() + 2, plaintext_len);
  return r;
}

std::string Nip44EncryptV2WithKeys(const uint8_t seckey32[32],
                                   const uint8_t pub32[32],
                                   const uint8_t nonce32[32],
                                   const std::string& plaintext) {
  uint8_t ck[32];
  if (!Nip44ConversationKey(seckey32, pub32, ck)) return std::string();
  return Nip44EncryptV2(ck, nonce32, plaintext);
}

Nip4xDecryptResult Nip44DecryptV2WithKeys(const uint8_t seckey32[32],
                                          const uint8_t pub32[32],
                                          const std::string& payload) {
  uint8_t ck[32];
  if (!Nip44ConversationKey(seckey32, pub32, ck)) {
    Nip4xDecryptResult r;
    r.error = Nip4xError::kBadKey;
    return r;
  }
  return Nip44DecryptV2(ck, payload);
}

// ----- NIP-04 -----

std::string Nip04Encrypt(const uint8_t seckey32[32], const uint8_t pub32[32],
                         const uint8_t iv16[16], const std::string& plaintext) {
  uint8_t shared_x[32];
  if (!EcdhSharedX(seckey32, pub32, shared_x)) return std::string();

  // Worst-case ciphertext size is plaintext + 16 (full PKCS#7 pad
  // block). Round up to next 16-byte boundary.
  const size_t pad = 16 - (plaintext.size() % 16);
  const size_t ct_len = plaintext.size() + pad;
  std::string ciphertext;
  ciphertext.resize(ct_len);
  Aes256CbcPkcs7Encrypt(
      shared_x, iv16, reinterpret_cast<const uint8_t*>(plaintext.data()),
      plaintext.size(), reinterpret_cast<uint8_t*>(&ciphertext[0]));

  std::string content =
      Base64Encode(reinterpret_cast<const uint8_t*>(ciphertext.data()), ct_len);
  content.append("?iv=");
  content.append(Base64Encode(iv16, 16));
  return content;
}

Nip4xDecryptResult Nip04Decrypt(const uint8_t seckey32[32],
                                const uint8_t pub32[32],
                                const std::string& content) {
  Nip4xDecryptResult r;
  const size_t marker = content.find("?iv=");
  if (marker == std::string::npos) {
    r.error = Nip4xError::kBadPayload;
    return r;
  }
  const std::string ct_b64 = content.substr(0, marker);
  const std::string iv_b64 = content.substr(marker + 4);
  std::string ct;
  std::string iv;
  if (!Base64Decode(ct_b64, ct) || !Base64Decode(iv_b64, iv)) {
    r.error = Nip4xError::kBadPayload;
    return r;
  }
  if (iv.size() != 16) {
    r.error = Nip4xError::kBadPayload;
    return r;
  }
  if (ct.empty() || (ct.size() % 16) != 0) {
    r.error = Nip4xError::kPlaintextRange;
    return r;
  }

  uint8_t shared_x[32];
  if (!EcdhSharedX(seckey32, pub32, shared_x)) {
    r.error = Nip4xError::kBadKey;
    return r;
  }

  std::string plain;
  plain.resize(ct.size());
  const size_t out_len = Aes256CbcPkcs7Decrypt(
      shared_x, reinterpret_cast<const uint8_t*>(iv.data()),
      reinterpret_cast<const uint8_t*>(ct.data()), ct.size(),
      reinterpret_cast<uint8_t*>(&plain[0]));
  if (out_len == SIZE_MAX) {
    r.error = Nip4xError::kBadPadding;
    return r;
  }
  plain.resize(out_len);
  r.ok = true;
  r.plaintext = std::move(plain);
  return r;
}

// ----- Dispatcher -----

EncryptionVariant ParseEncryptionTag(const std::string& token) {
  // NIP-47 advertises e.g. ["encryption", "nip44_v2 nip04"]. The
  // canonical tokens are `nip44_v2` (preferred — also seen as
  // `nip44` in some early-2024 wallets) and `nip04`. We accept both
  // spellings of the v2 token; anything else is treated as NIP-04,
  // which is the safest interop default for an absent or unknown
  // advertisement (see research §8 open question).
  if (token == "nip44_v2" || token == "nip44-v2" || token == "nip44") {
    return EncryptionVariant::kNip44V2;
  }
  return EncryptionVariant::kNip04;
}

std::string Encrypt(EncryptionVariant variant, const uint8_t seckey32[32],
                    const uint8_t pub32[32], const uint8_t* nonce_or_iv,
                    const std::string& plaintext) {
  switch (variant) {
    case EncryptionVariant::kNip44V2:
      return Nip44EncryptV2WithKeys(seckey32, pub32, nonce_or_iv, plaintext);
    case EncryptionVariant::kNip04:
      return Nip04Encrypt(seckey32, pub32, nonce_or_iv, plaintext);
  }
  return std::string();
}

Nip4xDecryptResult Decrypt(EncryptionVariant variant,
                           const uint8_t seckey32[32], const uint8_t pub32[32],
                           const std::string& content) {
  switch (variant) {
    case EncryptionVariant::kNip44V2:
      return Nip44DecryptV2WithKeys(seckey32, pub32, content);
    case EncryptionVariant::kNip04:
      return Nip04Decrypt(seckey32, pub32, content);
  }
  Nip4xDecryptResult r;
  r.error = Nip4xError::kBadPayload;
  return r;
}

}  // namespace nostr
}  // namespace btclock
