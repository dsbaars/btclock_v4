// Implementation of NIP-01 event verification.
//
// The canonical serializer is hand-rolled rather than cJSON-backed so
// the host-test build doesn't need cJSON in its dep set, and so the
// JSON output bytes are bit-identical to what the upstream signer
// produced — cJSON's number / escape choices are not guaranteed to
// match the NIP-01 minified form.
//
// The verifier uses the vendored libsecp256k1 with
// `secp256k1_context_static`. We never sign on this device, so a
// preallocated verify-only context is sufficient and saves the heap
// allocation that `secp256k1_context_create` would do.

#include "nostr/event_verify.hpp"

#include <cstdint>
#include <cstring>
#include <string>

#include "nostr/event.hpp"
#include "nostr/json_emit.hpp"
#include "secp256k1.h"
#include "secp256k1_extrakeys.h"
#include "secp256k1_schnorrsig.h"

namespace btclock {
namespace nostr {
namespace {

using ::btclock::nostr::json_emit::AppendString;
using ::btclock::nostr::json_emit::AppendUint;
// Aliased for call-site readability: existing serializer code below
// reads better with the JSON-specific noun than the broader namespace
// path.
inline void AppendJsonString(std::string& out, const std::string& s) {
  AppendString(out, s);
}

// SHA-256 — public-domain implementation lifted to constexpr-friendly
// form. We don't reuse mbedTLS' sha256 here because the host-test
// build doesn't link mbedTLS, and pulling it in just for the hash
// would balloon the test binary. Keeping it self-contained keeps the
// pure-logic property of this file.
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

// Decode a hex string of exactly `expected_len*2` characters. Returns
// false on any non-hex byte or length mismatch.
bool HexDecode(const std::string& hex, size_t expected_bytes, uint8_t* out) {
  if (hex.size() != expected_bytes * 2) return false;
  for (size_t i = 0; i < expected_bytes; ++i) {
    uint8_t b = 0;
    for (int j = 0; j < 2; ++j) {
      const char c = hex[2 * i + static_cast<size_t>(j)];
      uint8_t v = 0;
      if (c >= '0' && c <= '9')
        v = static_cast<uint8_t>(c - '0');
      else if (c >= 'a' && c <= 'f')
        v = static_cast<uint8_t>(10 + (c - 'a'));
      else if (c >= 'A' && c <= 'F')
        v = static_cast<uint8_t>(10 + (c - 'A'));
      else
        return false;
      b = static_cast<uint8_t>((b << 4) | v);
    }
    out[i] = b;
  }
  return true;
}

}  // namespace

std::string SerializeCanonical(const Event& ev) {
  std::string out;
  out.reserve(256 + ev.content.size());
  out.append("[0,");
  AppendJsonString(out, ev.pubkey);
  out.push_back(',');
  AppendUint(out, ev.created_at);
  out.push_back(',');
  AppendUint(out, ev.kind);
  out.push_back(',');
  // Tags array.
  out.push_back('[');
  for (size_t i = 0; i < ev.tags.size(); ++i) {
    if (i != 0) out.push_back(',');
    out.push_back('[');
    const auto& tag = ev.tags[i];
    for (size_t j = 0; j < tag.values.size(); ++j) {
      if (j != 0) out.push_back(',');
      AppendJsonString(out, tag.values[j]);
    }
    out.push_back(']');
  }
  out.push_back(']');
  out.push_back(',');
  AppendJsonString(out, ev.content);
  out.push_back(']');
  return out;
}

EventVerifyResult VerifyEvent(const Event& ev) {
  uint8_t id_bytes[32];
  uint8_t pk_bytes[32];
  uint8_t sig_bytes[64];
  if (!HexDecode(ev.id, 32, id_bytes)) return EventVerifyResult::kBadHexLength;
  if (!HexDecode(ev.pubkey, 32, pk_bytes))
    return EventVerifyResult::kBadHexLength;
  if (!HexDecode(ev.sig, 64, sig_bytes))
    return EventVerifyResult::kBadHexLength;

  // Recompute the canonical id and compare. A mismatch means either
  // the relay tampered with a field after the original signer hashed
  // it, or our serializer disagrees with the signer's. Either way,
  // refusing to forward the event upstream is the safe default.
  const std::string canon = SerializeCanonical(ev);
  Sha256Ctx sc;
  Sha256Init(sc);
  Sha256Update(sc, reinterpret_cast<const uint8_t*>(canon.data()),
               canon.size());
  uint8_t recomputed[32];
  Sha256Final(sc, recomputed);
  if (std::memcmp(recomputed, id_bytes, 32) != 0)
    return EventVerifyResult::kIdMismatch;

  secp256k1_xonly_pubkey xonly;
  if (!secp256k1_xonly_pubkey_parse(secp256k1_context_static, &xonly,
                                    pk_bytes)) {
    // Treat parse-fail as schnorr-invalid: it means the pubkey isn't a
    // valid x-only point on the curve, so no signature could verify
    // against it.
    return EventVerifyResult::kSchnorrInvalid;
  }
  if (secp256k1_schnorrsig_verify(secp256k1_context_static, sig_bytes, id_bytes,
                                  32, &xonly) != 1) {
    return EventVerifyResult::kSchnorrInvalid;
  }
  return EventVerifyResult::kOk;
}

}  // namespace nostr
}  // namespace btclock
