// BIP-340 schnorr sign without the secp256k1 ecmult_gen comb table.
//
// The signer composes:
//   * SHA-256 + BIP-340 tagged hash (inline, public-domain reference
//     impl) for the auxiliary, nonce and challenge digests.
//   * `secp256k1_ec_pubkey_tweak_mul` for both d*G and k*G — by
//     starting from a parsed G compressed point and multiplying. The
//     ecmult (verify-side) table is already linked for schnorr verify,
//     so this is a free ride.
//   * `secp256k1_ec_seckey_negate` / `_tweak_add` / `_tweak_mul` for
//     scalar arithmetic mod n. All three are constant-time and live in
//     the same translation units secp256k1 already pulls in for
//     `secp256k1_ec_seckey_verify`.
//
// What this file does NOT pull in: `precomputed_ecmult_gen.c` and the
// `ecmult_gen_*` machinery. With the comb table at 22 KiB (default
// COMB_BLOCKS=11 / COMB_TEETH=6 in upstream v0.7.0), avoiding it keeps
// Rev A's OTA headroom intact. See bd btclock_v4-lwf.2 for sizing.
//
// All primitives are deterministic and side-effect free; the only
// non-determinism enters via `aux_rand32` and the caller-supplied
// `created_at` field. Suitable for host tests (BIP-340 vectors are
// embedded in test_event_sign.cpp).

#include "nostr/event_sign.hpp"

#include <cstdint>
#include <cstring>
#include <string>

#include "nostr/event.hpp"
#include "nostr/event_verify.hpp"
#include "secp256k1.h"

namespace btclock {
namespace nostr {
namespace {

// secp256k1 generator G in compressed form (02 || G_x). Used as the
// starting point for both d*G and k*G via pubkey_tweak_mul. Public
// constant; copied from the BIP-340 spec.
constexpr uint8_t kG_Compressed[33] = {
    0x02, 0x79, 0xbe, 0x66, 0x7e, 0xf9, 0xdc, 0xbb, 0xac, 0x55, 0xa0,
    0x62, 0x95, 0xce, 0x87, 0x0b, 0x07, 0x02, 0x9b, 0xfc, 0xdb, 0x2d,
    0xce, 0x28, 0xd9, 0x59, 0xf2, 0x81, 0x5b, 0x16, 0xf8, 0x17, 0x98};

// Secp256k1 curve order n, big-endian. Used for the (rare) mod-n
// reduce after a tagged-hash output.
constexpr uint8_t kN[32] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe,
                            0xba, 0xae, 0xdc, 0xe6, 0xaf, 0x48, 0xa0, 0x3b,
                            0xbf, 0xd2, 0x5e, 0x8c, 0xd0, 0x36, 0x41, 0x41};

// ---------------------- SHA-256 ------------------------------------
//
// Same shape as `event_verify.cpp` / `nip4x.cpp`. Kept self-contained
// so the host-test build doesn't reach into mbedTLS. The duplication
// is a known follow-up — see bd notes on a future internal_crypto.hpp
// extraction.

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

// BIP-340 tagged hash: SHA256( SHA256(tag) || SHA256(tag) || msg ).
// Caller passes the tag bytes (no NUL) and the message; we hash both
// the prefix and the message in one streaming pass.
void TaggedHash(const char* tag, size_t tag_len, const uint8_t* msg,
                size_t msg_len, uint8_t out[32]) {
  uint8_t tag_hash[32];
  Sha256(reinterpret_cast<const uint8_t*>(tag), tag_len, tag_hash);
  Sha256Ctx c;
  Sha256Init(c);
  Sha256Update(c, tag_hash, 32);
  Sha256Update(c, tag_hash, 32);
  Sha256Update(c, msg, msg_len);
  Sha256Final(c, out);
}

// ----------------- 256-bit big-endian mod-n reduce -----------------
//
// `secp256k1_ec_seckey_verify` rejects scalars >= n. For tagged-hash
// outputs that fall outside [1, n-1] we need to reduce them to [0,
// n-1] by conditional subtraction. Probability of triggering the
// subtraction branch is ~2^-128; correctness still requires the
// branch.

// Returns 1 iff a >= n (lex compare, big-endian).
int Ge256(const uint8_t a[32], const uint8_t b[32]) {
  for (size_t i = 0; i < 32; ++i) {
    if (a[i] != b[i]) return a[i] > b[i] ? 1 : 0;
  }
  return 1;
}

// a = a - b (mod 2^256). Caller has verified a >= b.
void Sub256(uint8_t a[32], const uint8_t b[32]) {
  int borrow = 0;
  for (int i = 31; i >= 0; --i) {
    int v = static_cast<int>(a[i]) - static_cast<int>(b[i]) - borrow;
    if (v < 0) {
      v += 256;
      borrow = 1;
    } else {
      borrow = 0;
    }
    a[i] = static_cast<uint8_t>(v);
  }
}

bool IsZero32(const uint8_t a[32]) {
  uint8_t acc = 0;
  for (size_t i = 0; i < 32; ++i) acc |= a[i];
  return acc == 0;
}

// Reduce `a` mod n; returns false if the reduced value is zero
// (caller treats as a sign-side abort per BIP-340).
bool ReduceModN(uint8_t a[32]) {
  if (Ge256(a, kN)) Sub256(a, kN);
  return !IsZero32(a);
}

// ---------------- d*G via parse(G) + tweak_mul ---------------------
//
// Output: 32-byte x coordinate of d*G + parity bit of y. We need the
// y-parity to decide whether to negate d (BIP-340 step 3) and k (step
// 8).
bool ScalarMulG(const uint8_t scalar32[32], uint8_t out_x[32],
                bool& y_is_odd) {
  secp256k1_pubkey p;
  if (!secp256k1_ec_pubkey_parse(secp256k1_context_static, &p, kG_Compressed,
                                 sizeof(kG_Compressed))) {
    return false;
  }
  if (!secp256k1_ec_pubkey_tweak_mul(secp256k1_context_static, &p, scalar32)) {
    return false;
  }
  uint8_t ser[65];
  size_t len = sizeof(ser);
  if (!secp256k1_ec_pubkey_serialize(secp256k1_context_static, ser, &len, &p,
                                     SECP256K1_EC_UNCOMPRESSED)) {
    return false;
  }
  // ser = 0x04 || X (32B) || Y (32B). Y's last byte LSB is its parity.
  std::memcpy(out_x, ser + 1, 32);
  y_is_odd = (ser[64] & 0x01u) != 0u;
  return true;
}

void HexLowercase32(const uint8_t in[32], std::string& out) {
  static constexpr char kHex[] = "0123456789abcdef";
  out.resize(64);
  for (size_t i = 0; i < 32; ++i) {
    out[2 * i] = kHex[(in[i] >> 4) & 0xfu];
    out[2 * i + 1] = kHex[in[i] & 0xfu];
  }
}

void HexLowercase64(const uint8_t in[64], std::string& out) {
  static constexpr char kHex[] = "0123456789abcdef";
  out.resize(128);
  for (size_t i = 0; i < 64; ++i) {
    out[2 * i] = kHex[(in[i] >> 4) & 0xfu];
    out[2 * i + 1] = kHex[in[i] & 0xfu];
  }
}

}  // namespace

EventSignError DerivePubkeyXOnly(const uint8_t seckey32[32],
                                 uint8_t out_xonly32[32]) {
  if (!secp256k1_ec_seckey_verify(secp256k1_context_static, seckey32)) {
    return EventSignError::kBadSeckey;
  }
  bool y_is_odd = false;
  if (!ScalarMulG(seckey32, out_xonly32, y_is_odd)) {
    return EventSignError::kBadSeckey;
  }
  return EventSignError::kOk;
}

EventSignError SchnorrSign(const uint8_t seckey32[32], const uint8_t msg32[32],
                           const uint8_t aux_rand32[32], uint8_t out_sig64[64]) {
  // Step 1: validate seckey.
  if (!secp256k1_ec_seckey_verify(secp256k1_context_static, seckey32)) {
    return EventSignError::kBadSeckey;
  }

  // Step 2-3: P = d'*G; if P.y is odd, d = n - d'.
  uint8_t d[32];
  std::memcpy(d, seckey32, 32);
  uint8_t P_x[32];
  bool p_y_odd = false;
  if (!ScalarMulG(d, P_x, p_y_odd)) return EventSignError::kBadSeckey;
  if (p_y_odd) {
    if (!secp256k1_ec_seckey_negate(secp256k1_context_static, d)) {
      return EventSignError::kBadSeckey;
    }
  }

  // Step 4: t = bytes(d) XOR tagged_hash("BIP0340/aux", a).
  uint8_t aux_hash[32];
  TaggedHash("BIP0340/aux", 11, aux_rand32, 32, aux_hash);
  uint8_t t[32];
  for (size_t i = 0; i < 32; ++i) {
    t[i] = static_cast<uint8_t>(d[i] ^ aux_hash[i]);
  }

  // Step 5: rand = tagged_hash("BIP0340/nonce", t || bytes(P) || m).
  uint8_t nonce_in[96];
  std::memcpy(nonce_in, t, 32);
  std::memcpy(nonce_in + 32, P_x, 32);
  std::memcpy(nonce_in + 64, msg32, 32);
  uint8_t rand[32];
  TaggedHash("BIP0340/nonce", 13, nonce_in, sizeof(nonce_in), rand);

  // Step 6: k' = int(rand) mod n; fail on zero.
  if (!ReduceModN(rand)) return EventSignError::kSignFailed;

  // Step 7-8: R = k'*G; if R.y odd, k = n - k'.
  uint8_t k[32];
  std::memcpy(k, rand, 32);
  uint8_t R_x[32];
  bool r_y_odd = false;
  if (!ScalarMulG(k, R_x, r_y_odd)) return EventSignError::kSignFailed;
  if (r_y_odd) {
    if (!secp256k1_ec_seckey_negate(secp256k1_context_static, k)) {
      return EventSignError::kSignFailed;
    }
  }

  // Step 9: e = int(tagged_hash("BIP0340/challenge", bytes(R) ||
  // bytes(P) || m)) mod n.
  uint8_t chal_in[96];
  std::memcpy(chal_in, R_x, 32);
  std::memcpy(chal_in + 32, P_x, 32);
  std::memcpy(chal_in + 64, msg32, 32);
  uint8_t e[32];
  TaggedHash("BIP0340/challenge", 17, chal_in, sizeof(chal_in), e);
  if (!ReduceModN(e)) return EventSignError::kSignFailed;

  // Step 10: sig = bytes(R) || ((k + e*d) mod n).
  // We need k + e*d. seckey_tweak_mul writes a := a*b — so use a copy.
  uint8_t ed[32];
  std::memcpy(ed, e, 32);
  if (!secp256k1_ec_seckey_tweak_mul(secp256k1_context_static, ed, d)) {
    return EventSignError::kSignFailed;
  }
  if (!secp256k1_ec_seckey_tweak_add(secp256k1_context_static, k, ed)) {
    return EventSignError::kSignFailed;
  }

  std::memcpy(out_sig64, R_x, 32);
  std::memcpy(out_sig64 + 32, k, 32);
  return EventSignError::kOk;
}

EventSignError SignEvent(const uint8_t seckey32[32],
                         const uint8_t aux_rand32[32], Event& ev) {
  // Fill pubkey first so SerializeCanonical sees the right value.
  uint8_t pk_xonly[32];
  if (auto err = DerivePubkeyXOnly(seckey32, pk_xonly);
      err != EventSignError::kOk) {
    return err;
  }
  HexLowercase32(pk_xonly, ev.pubkey);

  // Recompute id = sha256(canonical).
  const std::string canon = SerializeCanonical(ev);
  uint8_t id[32];
  Sha256(reinterpret_cast<const uint8_t*>(canon.data()), canon.size(), id);
  HexLowercase32(id, ev.id);

  uint8_t sig[64];
  if (auto err = SchnorrSign(seckey32, id, aux_rand32, sig);
      err != EventSignError::kOk) {
    return err;
  }
  HexLowercase64(sig, ev.sig);
  return EventSignError::kOk;
}

}  // namespace nostr
}  // namespace btclock
