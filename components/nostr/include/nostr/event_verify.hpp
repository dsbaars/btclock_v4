// NIP-01 event verifier — schnorr signature + id integrity.
//
// Computes the canonical serialization per https://github.com/nostr-
// protocol/nips/blob/master/01.md (the array
// `[0, pubkey, created_at, kind, tags, content]` with no whitespace
// and a fixed escape set), sha256s it, compares against `ev.id`, and
// then verifies the BIP-340 schnorr `sig` over (id, pubkey) using the
// vendored libsecp256k1.
//
// Pure logic — no ESP-IDF, no allocator beyond std::string. Both the
// firmware and the host-test build link the same vendored
// libsecp256k1, so the verifier behaves identically on-target and on
// the host.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "nostr/event.hpp"

namespace btclock {
namespace nostr {

// Reasons a verification can fail. Surfaced as an enum so callers can
// log a stable tag and tests can pin the exact failure mode without
// scraping log strings.
enum class EventVerifyResult : uint8_t {
  kOk = 0,
  kBadHexLength,    // id/pubkey/sig wrong length or non-hex
  kIdMismatch,      // recomputed sha256(canonical) != ev.id
  kSchnorrInvalid,  // signature failed BIP-340 verification
};

// Verify a Nostr event end-to-end. Returns kOk iff every check passes.
// On failure the function makes a best-effort early return — id length
// checks happen before the schnorr call so we don't waste cycles on a
// frame that's structurally wrong.
EventVerifyResult VerifyEvent(const Event& ev);

// Canonical serialization helper exposed for host tests. Builds the
// NIP-01 `[0, pubkey, created_at, kind, tags, content]` array as a
// minified JSON string. UTF-8 pass-through; the escape set is the
// NIP-01 mandated subset (`"`, `\`, `\n`, `\r`, `\t`, `\b`, `\f`).
// All other bytes — including the `/` and pre-existing `\u` escapes
// in the parsed-side content — are written verbatim. This matches
// what every well-behaved Nostr signer produces.
std::string SerializeCanonical(const Event& ev);

}  // namespace nostr
}  // namespace btclock
