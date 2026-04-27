// Nostr event + envelope types (NIP-01).
//
// A relay → client message is a JSON array with a text frame type:
//
//   ["EVENT",  "<sub-id>", {<event object>}]
//   ["EOSE",   "<sub-id>"]
//   ["CLOSED", "<sub-id>", "<reason>"]
//   ["NOTICE", "<human-readable message>"]
//   ["OK",     "<event-id>", <bool>, "<reason>"]
//
// Client → relay:
//
//   ["REQ",    "<sub-id>", {<filter>}, ...]
//   ["CLOSE",  "<sub-id>"]
//
// We model the event object as a plain struct of fields we actually
// consume; unknown fields are silently dropped. Tags are stored as a
// vector of string-vectors — the first element is the tag name, the rest
// are values per NIP-01. Signature integrity is enforced one layer up
// at the data-source seam (`nostr/event_verify.hpp` — schnorr verify
// over the recomputed canonical id), so consumers that read this
// struct can treat the fields as authentic.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace btclock {
namespace nostr {

// Event kind constants used by this firmware.
constexpr uint32_t kKindAppData = 30078;    // NIP-78 (btclock publisher)
constexpr uint32_t kKindZapReceipt = 9735;  // NIP-57 zap receipt

struct Tag {
  // First element is the tag name (e.g. "d", "p", "bolt11"), subsequent
  // elements are the typed values. Per NIP-01 the sequence may be empty.
  std::vector<std::string> values;

  // Convenience — value[0] is always present if `values` is non-empty.
  const std::string& name() const { return values[0]; }
  // Typed value at index n (1-based in NIP-01 terms), or "" if absent.
  const std::string& at(size_t idx) const {
    static const std::string kEmpty;
    return idx < values.size() ? values[idx] : kEmpty;
  }
};

struct Event {
  std::string id;
  std::string pubkey;
  uint64_t created_at = 0;
  uint32_t kind = 0;
  std::vector<Tag> tags;
  std::string content;
  std::string
      sig;  // hex, 128 chars; verified by VerifyEvent (event_verify.hpp)

  // Find the first tag whose name matches `name`; returns nullptr if
  // absent. NIP-01 allows multiple tags with the same name — callers
  // that need all values must walk `tags` themselves.
  const Tag* FindTag(const std::string& name) const;

  // Shortcut — returns the first value of the first matching tag, or
  // "" if none. Equivalent to `FindTag(name)->at(1)` with a safe empty.
  const std::string& TagValue(const std::string& name) const;
};

// A single relay → client envelope decoded to a tagged variant. Only
// the shapes this firmware cares about are modelled; NOTICE / OK are
// decoded as `other` so the caller can log them raw.
enum class EnvelopeType : uint8_t {
  kUnknown,
  kEvent,
  kEose,
  kClosed,
  kNotice,
  kOk,
};

struct Envelope {
  EnvelopeType type = EnvelopeType::kUnknown;
  std::string sub_id;   // set for kEvent, kEose, kClosed
  std::string message;  // set for kNotice, kClosed reason, kOk reason
  Event event;          // populated iff type == kEvent
};

}  // namespace nostr
}  // namespace btclock
