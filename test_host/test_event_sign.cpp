// Host tests for nostr/event_sign — BIP-340 schnorr signing without
// the secp256k1 ecmult_gen comb table.
//
// The signer is hand-rolled on top of seckey/pubkey-tweak public API
// calls (see components/nostr/src/event_sign.cpp). Correctness pin:
// run the four BIP-340 spec vectors that include a secret key
// (indices 0-3 in `bitcoin/bips/bip-0340/test-vectors.csv`) through
// our `SchnorrSign` and check the output is bit-identical with the
// expected signature in the vector. Aux-rand is part of the spec
// vector — we can't fudge it because BIP-340 derives the nonce from
// `tag_hash(d XOR aux_hash, P.x, msg)`, so two different signers with
// the same aux+key+msg MUST produce the same sig.
//
// Then a roundtrip test: sign a NIP-01 event with `SignEvent` and
// verify it with `VerifyEvent`. Covers the canonical-id recompute and
// pubkey/x-only hex encoding plus the schnorr path end-to-end with
// our own bytes, not a hand-minted fixture.

#include <array>
#include <cstring>
#include <string>

#include "doctest.h"
#include "nostr/event.hpp"
#include "nostr/event_sign.hpp"
#include "nostr/event_verify.hpp"

using namespace btclock::nostr;

namespace {

// Decode `hex` (any length, lowercase or uppercase) into `out`. The
// fixture vectors are hex-encoded so embedded `\xNN` would be a worse
// portability story.
std::array<uint8_t, 32> Hex32(const char* hex) {
  std::array<uint8_t, 32> out{};
  for (size_t i = 0; i < 32; ++i) {
    auto nibble = [](char c) -> uint8_t {
      if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
      if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + c - 'a');
      if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(10 + c - 'A');
      return 0;
    };
    out[i] = static_cast<uint8_t>((nibble(hex[2 * i]) << 4u) |
                                  nibble(hex[2 * i + 1]));
  }
  return out;
}

std::array<uint8_t, 64> Hex64(const char* hex) {
  std::array<uint8_t, 64> out{};
  for (size_t i = 0; i < 64; ++i) {
    auto nibble = [](char c) -> uint8_t {
      if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
      if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + c - 'a');
      if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(10 + c - 'A');
      return 0;
    };
    out[i] = static_cast<uint8_t>((nibble(hex[2 * i]) << 4u) |
                                  nibble(hex[2 * i + 1]));
  }
  return out;
}

struct Vector {
  const char* sk;
  const char* pk;   // expected x-only pubkey from sk
  const char* aux;  // 32-byte aux randomness
  const char* msg;  // 32-byte message
  const char* sig;  // expected 64-byte schnorr sig
};

// BIP-340 spec vectors that exercise the sign side (indices 0-3 in
// bip-0340/test-vectors.csv). The vector at index 0 is the smallest
// possible — sk = 3, msg / aux all zero — and tests the negate-d
// branch (P_3 has odd y). Index 3 puts the message at the curve
// boundary to catch mod-p / mod-n confusion.
constexpr Vector kVectors[] = {
    // 0: sk=3, msg=0, aux=0
    {"0000000000000000000000000000000000000000000000000000000000000003",
     "f9308a019258c31049344f85f89d5229b531c845836f99b08601f113bce036f9",
     "0000000000000000000000000000000000000000000000000000000000000000",
     "0000000000000000000000000000000000000000000000000000000000000000",
     "e907831f80848d1069a5371b402410364bdf1c5f8307b0084c55f1ce2dca8215"
     "25f66a4a85ea8b71e482a74f382d2ce5ebeee8fdb2172f477df4900d310536c0"},
    // 1: textbook digits of pi as the message, aux = 1
    {"b7e151628aed2a6abf7158809cf4f3c762e7160f38b4da56a784d9045190cfef",
     "dff1d77f2a671c5f36183726db2341be58feae1da2deced843240f7b502ba659",
     "0000000000000000000000000000000000000000000000000000000000000001",
     "243f6a8885a308d313198a2e03707344a4093822299f31d0082efa98ec4e6c89",
     "6896bd60eeae296db48a229ff71dfe071bde413e6d43f917dc8dcf8c78de3341"
     "8906d11ac976abccb20b091292bff4ea897efcb639ea871cfa95f6de339e4b0a"},
    // 2: full 256-bit msg + aux
    {"c90fdaa22168c234c4c6628b80dc1cd129024e088a67cc74020bbea63b14e5c9",
     "dd308afec5777e13121fa72b9cc1b7cc0139715309b086c960e18fd969774eb8",
     "c87aa53824b4d7ae2eb035a2b5bbbccc080e76cdc6d1692c4b0b62d798e6d906",
     "7e2d58d8b3bcdf1abadec7829054f90dda9805aab56c77333024b9d0a508b75c",
     "5831aaeed7b44bb74e5eab94ba9d4294c49bcf2a60728d8b4c200f50dd313c1b"
     "ab745879a5ad954a72c45a91c3a51d3c7adea98d82f8481e0e1e03674a6f3fb7"},
    // 3: msg = aux = all-FF (catches mod-p / mod-n confusion)
    {"0b432b2677937381aef05bb02a66ecd012773062cf3fa2549e44f58ed2401710",
     "25d1dff95105f5253c4022f628a996ad3a0d95fbf21d468a1b33f8c160d8f517",
     "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
     "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
     "7eb0509757e246f19449885651611cb965ecc1a187dd51b64fda1edc9637d5ec"
     "97582b9cb13db3933705b32ba982af5af25fd78881ebb32771fc5922efc66ea3"},
};

}  // namespace

TEST_CASE("DerivePubkeyXOnly matches BIP-340 expected pubkey") {
  for (const auto& v : kVectors) {
    const auto sk = Hex32(v.sk);
    const auto expected_pk = Hex32(v.pk);
    uint8_t pk[32];
    REQUIRE(DerivePubkeyXOnly(sk.data(), pk) == EventSignError::kOk);
    CHECK(std::memcmp(pk, expected_pk.data(), 32) == 0);
  }
}

TEST_CASE("SchnorrSign reproduces BIP-340 spec signature vectors") {
  for (const auto& v : kVectors) {
    const auto sk = Hex32(v.sk);
    const auto aux = Hex32(v.aux);
    const auto msg = Hex32(v.msg);
    const auto expected_sig = Hex64(v.sig);
    uint8_t sig[64];
    REQUIRE(SchnorrSign(sk.data(), msg.data(), aux.data(), sig) ==
            EventSignError::kOk);
    CHECK(std::memcmp(sig, expected_sig.data(), 64) == 0);
  }
}

TEST_CASE("DerivePubkeyXOnly rejects 0 and >=n seckeys") {
  uint8_t out[32];

  // Zero seckey.
  uint8_t zero[32]{};
  CHECK(DerivePubkeyXOnly(zero, out) == EventSignError::kBadSeckey);

  // n itself (one above the largest legal scalar).
  uint8_t n_bytes[32] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                         0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe,
                         0xba, 0xae, 0xdc, 0xe6, 0xaf, 0x48, 0xa0, 0x3b,
                         0xbf, 0xd2, 0x5e, 0x8c, 0xd0, 0x36, 0x41, 0x41};
  CHECK(DerivePubkeyXOnly(n_bytes, out) == EventSignError::kBadSeckey);

  // n + 1 (clearly >= n).
  n_bytes[31] = 0x42;
  CHECK(DerivePubkeyXOnly(n_bytes, out) == EventSignError::kBadSeckey);
}

TEST_CASE("SignEvent + VerifyEvent roundtrip on a kind-23194 NWC request") {
  // Build a synthetic NIP-47 kind-23194 NWC request. created_at is
  // fixed so the canonical-id recompute is deterministic against any
  // future test re-run; content is the NIP-04 / NIP-44 placeholder so
  // the field path through SerializeCanonical exercises base64-style
  // text.
  Event ev;
  ev.created_at = 1747000000;
  ev.kind = 23194;
  Tag p_tag;
  p_tag.values.push_back("p");
  p_tag.values.push_back(
      "b889ff5b1513b641e2a139f661a661364979c5beee91842f8f0ef42ab558e9d4");
  ev.tags.push_back(p_tag);
  Tag enc_tag;
  enc_tag.values.push_back("encryption");
  enc_tag.values.push_back("nip44_v2");
  ev.tags.push_back(enc_tag);
  ev.content =
      "AgEhqzCAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

  // BIP-340 vector 1's seckey, aux=0. Deterministic + lets the roundtrip
  // assert behave like a regression vector if the canonical serializer
  // ever drifts.
  const auto sk = Hex32(
      "b7e151628aed2a6abf7158809cf4f3c762e7160f38b4da56a784d9045190cfef");
  const auto aux = Hex32(
      "0000000000000000000000000000000000000000000000000000000000000000");
  REQUIRE(SignEvent(sk.data(), aux.data(), ev) == EventSignError::kOk);

  // Fields are filled in.
  CHECK(ev.pubkey ==
        "dff1d77f2a671c5f36183726db2341be58feae1da2deced843240f7b502ba659");
  CHECK(ev.id.size() == 64);
  CHECK(ev.sig.size() == 128);

  // The whole point — the just-signed event must pass VerifyEvent.
  CHECK(VerifyEvent(ev) == EventVerifyResult::kOk);
}

TEST_CASE("SignEvent: tampering with content after signing breaks verify") {
  Event ev;
  ev.created_at = 1747000000;
  ev.kind = 1;
  ev.content = "hello world";

  const auto sk = Hex32(
      "b7e151628aed2a6abf7158809cf4f3c762e7160f38b4da56a784d9045190cfef");
  const auto aux = Hex32(
      "0000000000000000000000000000000000000000000000000000000000000000");
  REQUIRE(SignEvent(sk.data(), aux.data(), ev) == EventSignError::kOk);
  REQUIRE(VerifyEvent(ev) == EventVerifyResult::kOk);

  ev.content = "hello world!";
  CHECK(VerifyEvent(ev) == EventVerifyResult::kIdMismatch);
}

TEST_CASE("SignEvent: zero seckey rejected") {
  Event ev;
  ev.created_at = 1;
  ev.kind = 1;
  uint8_t zero[32]{};
  CHECK(SignEvent(zero, zero, ev) == EventSignError::kBadSeckey);
}
