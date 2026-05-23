// NIP-04 + NIP-44 v2 encryption / decryption for NIP-47 Nostr Wallet
// Connect.
//
// This unit implements two interoperable payload formats:
//
//   * NIP-44 v2 — secp256k1 ECDH, HKDF-SHA256, ChaCha20, HMAC-SHA256,
//     base64. The forward-looking format; preferred whenever the
//     counterparty advertises support via the kind 13194 INFO event's
//     `encryption` tag.
//   * NIP-04 — secp256k1 ECDH, AES-256-CBC with PKCS#7 padding,
//     base64. Legacy interop fallback. Kept because a meaningful
//     fraction of NWC wallets in the field haven't shipped NIP-44 v2
//     yet (the `encryption` tag was added to NIP-47 in 2024).
//
// Crypto routing — host vs target:
//
//   * On ESP_PLATFORM (IDF target), the AES-256-CBC round work and
//     the SHA-256 core (used by NIP-44's HMAC + HKDF chain) route
//     through mbedTLS so the ESP32-S3 hardware AES + SHA peripherals
//     do the heavy lifting (CONFIG_MBEDTLS_HARDWARE_{AES,SHA}=y).
//     The HMAC/HKDF wrappers above the SHA core stay in this file —
//     the IDF mbedtls md.h dispatcher mallocs an inner ctx per call,
//     which we don't want on the per-notify path.
//   * ChaCha20 stays hand-rolled on both host and target. Mbedtls's
//     chacha20.c is the same RFC 8439 textbook code and the ESP32-S3
//     has no ChaCha20 accelerator; routing through mbedtls would
//     force `CONFIG_MBEDTLS_CHACHA20_C=y` (currently `n` by default)
//     which links a duplicate ~9 KiB software impl into every variant
//     — measurably worse on Rev A's 4 MB partition (≈8 % free).
//   * Base64 stays hand-rolled. mbedtls_base64_* is fine but pulls
//     mbedtls_base64.o which we'd otherwise leave out of the link.
//
// On the host (test_host/, no IDF / no mbedTLS), every primitive
// stays in its textbook software form so the official NIP-44 v2
// spec vectors run verbatim in CI without an ESP-IDF build. The
// HW and SW backends share the same nip4x.cpp public surface; the
// vectors validate the SW path, on-device NWC traffic stress-tests
// the HW path (see [[nwc-test-flow]]).
//
// ECDH uses the vendored libsecp256k1 via `secp256k1_ec_pubkey_tweak_mul`
// (already linked for schnorr verify). The shared-X coordinate is the
// raw 32-byte value, NOT the SHA-256 of the compressed point that
// libsecp256k1's stock ECDH module produces — both NIPs require the
// unhashed X.
//
// The crypto is *deterministic* at this layer: every encrypt entry
// point takes the nonce / IV from the caller. The NWC state machine
// is responsible for pulling randomness from `esp_fill_random` (on
// target) or the host RNG (in tests); see `NwcRandomBytes` once that
// component lands. Deterministic encrypts let us run the official
// NIP-44 v2 fixture's `payload` strings through `==` comparison in
// host tests.

#pragma once

#include <cstdint>
#include <string>

namespace btclock {
namespace nostr {

// Wallet-advertised encryption variant (NIP-47 kind 13194 INFO event
// `encryption` tag, added 2024). On the wire wallets advertise as e.g.
// `["encryption", "nip44_v2 nip04"]` — callers split by whitespace and
// pass each token through `ParseEncryptionTag`.
enum class EncryptionVariant : uint8_t {
  kNip04 = 0,
  kNip44V2 = 1,
};

// Stable error codes for decrypt failures. Surfaced through the
// `Nip4xDecryptResult::error` field so callers can log a tagged
// reason without scraping a message string. `kBadKey` covers both
// invalid private keys (zero / >= curve order) and invalid public
// keys (off-curve / on twist).
enum class Nip4xError : uint8_t {
  kOk = 0,
  kBadKey,          // ECDH refused (invalid privkey or pubkey)
  kBadPayload,      // base64 decode failed or wrong length
  kUnknownVersion,  // version byte not 0x02 (NIP-44 v2 only)
  kMacMismatch,     // HMAC-SHA256 over (nonce || ct) didn't match
  kBadPadding,      // NIP-44 padding scheme rejected the plaintext
                    //   or NIP-04 PKCS#7 padding was malformed
  kPlaintextRange,  // plaintext length outside [1, 65535] (NIP-44)
                    //   or ciphertext not a multiple of 16 (NIP-04)
};

struct Nip4xDecryptResult {
  bool ok = false;
  std::string plaintext;
  Nip4xError error = Nip4xError::kOk;
};

// ----------------------------------------------------------------- NIP-44 v2

// Compute the NIP-44 v2 conversation key from our private key and the
// peer's x-only (BIP-340) public key:
//
//   shared_x  = X-coord of (seckey * lift_x(pub32))
//   conv_key  = HKDF-Extract(salt="nip44-v2", IKM=shared_x)
//
// `lift_x` follows BIP-340 — the y coordinate is chosen such that y is
// even, equivalent to prepending 0x02 before parsing as a compressed
// secp256k1 point. Returns false if either key is invalid (privkey is
// zero or >= curve order, or pubkey x is not on the curve).
//
// The conversation key is constant for a given (us, them) pair across
// every message exchanged with that wallet — caching it across
// requests is the dominant optimization once a wallet is paired.
bool Nip44ConversationKey(const uint8_t seckey32[32], const uint8_t pub32[32],
                          uint8_t conversation_key32[32]);

// NIP-44 v2 encrypt with caller-supplied 32-byte nonce. The nonce
// must come from a CSPRNG and MUST NOT be reused across messages on
// the same conversation key (reuse would make both messages
// decryptable; the long-term key survives, but the privacy gain of
// NIP-44 over NIP-04 evaporates).
//
// Returns the base64-encoded payload string. Returns empty string on
// failure — invalid plaintext length only, since both keys are
// already validated when the conversation key was derived. Plaintext
// must be 1..65535 bytes.
std::string Nip44EncryptV2(const uint8_t conversation_key32[32],
                           const uint8_t nonce32[32],
                           const std::string& plaintext);

// NIP-44 v2 decrypt. Performs constant-time MAC verification before
// touching the ciphertext. `payload` is the base64 string from the
// event's `.content` field (or the kind 23196 notification content).
Nip4xDecryptResult Nip44DecryptV2(const uint8_t conversation_key32[32],
                                  const std::string& payload);

// Convenience: compute the conversation key inline. Slower for
// repeated calls; the state machine should cache the conversation
// key per peer. Empty return on key failure or plaintext range.
std::string Nip44EncryptV2WithKeys(const uint8_t seckey32[32],
                                   const uint8_t pub32[32],
                                   const uint8_t nonce32[32],
                                   const std::string& plaintext);

Nip4xDecryptResult Nip44DecryptV2WithKeys(const uint8_t seckey32[32],
                                          const uint8_t pub32[32],
                                          const std::string& payload);

// ----------------------------------------------------------------- NIP-04

// NIP-04 encrypt. Output content format:
//
//   <base64(ciphertext)>?iv=<base64(iv)>
//
// IV is 16 bytes (AES block size), must come from a CSPRNG, must not
// repeat under the same shared key. Plaintext is encoded UTF-8 and
// padded with PKCS#7 before AES-256-CBC. Returns empty string on key
// failure.
std::string Nip04Encrypt(const uint8_t seckey32[32], const uint8_t pub32[32],
                         const uint8_t iv16[16], const std::string& plaintext);

// NIP-04 decrypt. `content` is the `<b64ct>?iv=<b64iv>` string.
Nip4xDecryptResult Nip04Decrypt(const uint8_t seckey32[32],
                                const uint8_t pub32[32],
                                const std::string& content);

// ----------------------------------------------------------------- Dispatcher

// Parse a single token from a NIP-47 kind 13194 `encryption` tag.
// Recognised tokens are `nip44_v2` (preferred) and `nip04` (fallback).
// Anything else returns kNip04 — the safest interop choice when the
// wallet doesn't advertise a known variant. See research §8.
EncryptionVariant ParseEncryptionTag(const std::string& token);

// Encrypt dispatcher. Routes to NIP-44 v2 or NIP-04 based on
// `variant`. `nonce_or_iv` is 32 bytes for NIP-44 v2 (full nonce) or
// 16 bytes for NIP-04 (AES IV) — the caller is responsible for
// sizing the buffer correctly. Returns empty string on failure.
std::string Encrypt(EncryptionVariant variant, const uint8_t seckey32[32],
                    const uint8_t pub32[32], const uint8_t* nonce_or_iv,
                    const std::string& plaintext);

// Decrypt dispatcher. Routes to NIP-44 v2 or NIP-04 based on
// `variant`. Both formats are self-describing in their content (NIP-44
// v2 by its version byte after base64 decode; NIP-04 by the literal
// `?iv=` substring), but the dispatcher trusts the caller — wallets
// only emit one or the other per conversation.
Nip4xDecryptResult Decrypt(EncryptionVariant variant,
                           const uint8_t seckey32[32], const uint8_t pub32[32],
                           const std::string& content);

}  // namespace nostr
}  // namespace btclock
