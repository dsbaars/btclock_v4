// Pure-logic helpers for parsing btclock WS v2 frame fields.
//
// The production data path decodes MessagePack via ArduinoJson and
// dispatches fields directly into a DataSnapshot. That path pulls in
// ESP-IDF headers and so cannot be unit-tested host-side.
//
// These helpers operate on plain JSON text — they cover the legacy v1
// WebSocket protocol (`/ws`, `/api/v1/ws`) which emits UTF-8 JSON frames
// like `{"bitcoin": "64211.53"}` or `{"blockfee2": 12.75}`. The MessagePack
// v2 path can also feed these if a caller first re-encodes to JSON, but
// in practice we keep them independent.
//
// The key property we rely on in the host tests: a synthetic
// `{"blockfee2": 12.75}` JSON frame yields 12.75 exactly (double
// precision, no rounding). No allocator / no ESP-IDF.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace btclock {
namespace parse {

// Skip ASCII whitespace starting at pos. Returns the first non-space index
// (or `text.size()` if none).
inline size_t SkipWs(const std::string& text, size_t pos) {
  while (pos < text.size()) {
    const char c = text[pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      ++pos;
    } else {
      break;
    }
  }
  return pos;
}

// Find `"<key>"` as an object key. Returns the index just past the
// terminating `"` of the key, or `std::string::npos` if not present.
// Ignores occurrences inside values (we only accept keys immediately
// preceded by `{` or `,`, optionally with whitespace).
//
// This is intentionally tiny: we don't support escaped characters in
// keys (ws-nostr-publish never emits any) and we don't handle nested
// objects correctly. Good enough for flat v1 frames.
inline size_t FindKey(const std::string& text, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  size_t from = 0;
  while (true) {
    const size_t hit = text.find(needle, from);
    if (hit == std::string::npos) return std::string::npos;
    // Walk back to the previous non-space char.
    size_t b = hit;
    while (b > 0) {
      --b;
      const char c = text[b];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
      if (c == '{' || c == ',') return hit + needle.size();
      break;
    }
    // Hit was inside a value, keep searching.
    from = hit + needle.size();
  }
}

// Extract the numeric value for an object key from a flat JSON frame.
// Returns true and writes to `out` iff the key is present and the value
// is a JSON number (optionally signed, optionally with a decimal point
// and/or an exponent). String-encoded numbers are rejected — use the
// `*AsString` variant if the server emits them quoted.
inline bool ExtractJsonNumber(const std::string& text, const std::string& key,
                              double& out) {
  const size_t after_key = FindKey(text, key);
  if (after_key == std::string::npos) return false;
  size_t p = SkipWs(text, after_key);
  if (p >= text.size() || text[p] != ':') return false;
  p = SkipWs(text, p + 1);
  if (p >= text.size()) return false;

  // Consume a JSON number: optional sign, digits, optional fractional
  // part, optional exponent.
  const size_t start = p;
  if (text[p] == '-' || text[p] == '+') ++p;
  bool any_digit = false;
  while (p < text.size() && text[p] >= '0' && text[p] <= '9') {
    ++p;
    any_digit = true;
  }
  if (p < text.size() && text[p] == '.') {
    ++p;
    while (p < text.size() && text[p] >= '0' && text[p] <= '9') {
      ++p;
      any_digit = true;
    }
  }
  if (p < text.size() && (text[p] == 'e' || text[p] == 'E')) {
    ++p;
    if (p < text.size() && (text[p] == '+' || text[p] == '-')) ++p;
    while (p < text.size() && text[p] >= '0' && text[p] <= '9') ++p;
  }
  if (!any_digit) return false;

  const std::string num = text.substr(start, p - start);
  char* end = nullptr;
  const double v = std::strtod(num.c_str(), &end);
  if (end != num.c_str() + num.size()) return false;
  out = v;
  return true;
}

// Integer overload — parses the same way, truncates toward zero.
inline bool ExtractJsonInt(const std::string& text, const std::string& key,
                           int64_t& out) {
  double d = 0;
  if (!ExtractJsonNumber(text, key, d)) return false;
  out = static_cast<int64_t>(d);
  return true;
}

}  // namespace parse
}  // namespace btclock
