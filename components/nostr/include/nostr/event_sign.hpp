// BIP-340 schnorr sign + NIP-01 event signing.
//
// This is the sign-side counterpart to `event_verify.hpp`. It exists
// so the firmware can publish NIP-47 NWC requests (kind 23194) without
// linking secp256k1's `ecmult_gen` precomputed table.
//
// Rationale for the table-free implementation:
//
//   The default upstream comb config (`COMB_BLOCKS=11`, `COMB_TEETH=6`)
//   produces a ~22 KiB rodata table — bigger than the Rev A OTA
//   headroom can absorb on top of the encryption module. Every
//   primitive the signer needs is already available through the
//   public secp256k1 API which is on the verify-only (ecmult) side
//   already linked for `secp256k1_schnorrsig_verify`:
//
//     * scalar * G   — parse `G` as a compressed pubkey, then
//                       `secp256k1_ec_pubkey_tweak_mul`. Cost: one
//                       ecmult per call. Slower than the gen-table
//                       path, irrelevant at NWC's <1 Hz cadence.
//     * scalar + n / scalar * n   — `secp256k1_ec_seckey_tweak_add` /
//                                    `secp256k1_ec_seckey_tweak_mul`.
//     * scalar negate              — `secp256k1_ec_seckey_negate`.
//
//   The result is a sign-side that pays ~1.5 KiB code (this TU) and
//   keeps `precomputed_ecmult_gen.c` DCE'd. See bd btclock_v4-lwf.2.
//
// Hash: SHA-256 + BIP-340 tagged hashes (`SHA256(SHA256(tag) ||
// SHA256(tag) || data)`), implemented inline here to keep the host-test
// build self-contained — same pattern as `event_verify.cpp` and
// `nip4x.cpp`.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "nostr/event.hpp"

namespace btclock {
namespace nostr {

enum class EventSignError : uint8_t {
  kOk = 0,
  kBadSeckey,   // 0 or >= n
  kSignFailed,  // negligible — k+e*d == 0 or e == 0 mod n
};

// Derive the x-only (BIP-340) 32-byte public key from a 32-byte
// secret key. Returns kBadSeckey if `seckey32` is 0 or >= curve order.
// `out_xonly32` receives the X coordinate in big-endian.
//
// Uses `secp256k1_ec_pubkey_tweak_mul(G, seckey)` rather than the
// gen-table path so this routine compiles without
// `precomputed_ecmult_gen.c` being linked. See the file header for
// the size rationale.
EventSignError DerivePubkeyXOnly(const uint8_t seckey32[32],
                                 uint8_t out_xonly32[32]);

// BIP-340 schnorr-sign a 32-byte message hash with the given secret
// key. `aux_rand32` is the auxiliary randomness (32 bytes). Zero is
// permitted and produces a deterministic-but-not-RFC-6979 signature,
// which is fine for our use case (we'll seed from `esp_fill_random`
// on target / RNG in tests). Returns kBadSeckey on invalid seckey
// and kSignFailed on the (vacuous) edge cases.
EventSignError SchnorrSign(const uint8_t seckey32[32], const uint8_t msg32[32],
                           const uint8_t aux_rand32[32], uint8_t out_sig64[64]);

// Sign a NIP-01 event in place: computes the canonical id (sha256 of
// the canonical serialization), schnorr-signs the id with `seckey32`,
// and fills in `ev.pubkey` (lowercase hex), `ev.id` (lowercase hex)
// and `ev.sig` (lowercase hex). `ev.created_at`, `ev.kind`,
// `ev.tags`, `ev.content` must be set by the caller; the function
// does not touch any other field.
//
// `aux_rand32` is forwarded to `SchnorrSign`. Pass a fresh CSPRNG
// chunk per call; reuse is harmless for BIP-340 but defeats the
// privacy bump aux-rand was designed to provide.
EventSignError SignEvent(const uint8_t seckey32[32],
                         const uint8_t aux_rand32[32], Event& ev);

}  // namespace nostr
}  // namespace btclock
