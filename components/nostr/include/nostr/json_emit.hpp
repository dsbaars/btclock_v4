// Tiny JSON emitter helpers shared between the relay-frame builder
// (NWC client.cpp) and the canonical-event serialiser
// (event_verify.cpp). Out-of-line definitions live in json_emit.cpp
// so the linker emits a single copy across the firmware image
// instead of one per call-site TU — saved a few hundred bytes of
// duplicated escape-switch code on Rev A's 4 MB app partition.

#pragma once

#include <cstdint>
#include <string>

namespace btclock {
namespace nostr {
namespace json_emit {

// Append `s` as a JSON string literal (with surrounding quotes) using
// the minimal NIP-01 escape set. UTF-8 bytes pass through unchanged —
// the spec hashes the same byte sequence the signer emitted, so any
// re-encoding here must be byte-identical to the original.
void AppendString(std::string& out, const std::string& s);

// Decimal-integer appender. Avoids snprintf locale surprises and the
// Ryu/dtoa tables `std::to_string` would drag in.
void AppendUint(std::string& out, uint64_t n);

}  // namespace json_emit
}  // namespace nostr
}  // namespace btclock
