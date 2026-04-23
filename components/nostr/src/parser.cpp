// NIP-01 envelope parser — pure-logic implementation.
//
// Intentionally hand-rolled rather than cJSON-backed so the same file
// compiles cleanly in the host-test build (no ESP-IDF headers, no
// managed_components fetch). cJSON remains available for components
// that want it via `REQUIRES json` in CMake.
//
// Scope limits (documented in parser.hpp): no \u escape decoding, no
// deep recursion (we only walk one level of tag arrays), no signature
// verification. This is sufficient for the kind 30078 + kind 9735
// frames we consume.

#include "nostr/parser.hpp"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "data_core/snapshot.hpp"
#include "nostr/event.hpp"

namespace btclock {
namespace nostr {
namespace {

// --- Tiny JSON walker -------------------------------------------------

size_t SkipWs(const std::string& s, size_t p) {
  while (p < s.size()) {
    const char c = s[p];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      ++p;
    } else {
      break;
    }
  }
  return p;
}

// Parse a JSON string starting at `p` (which must point at the opening
// `"`). On success returns the index just past the closing `"` and
// writes the decoded contents to `out`. On failure returns
// std::string::npos.
size_t ParseString(const std::string& s, size_t p, std::string& out) {
  out.clear();
  if (p >= s.size() || s[p] != '"') return std::string::npos;
  ++p;
  while (p < s.size()) {
    const char c = s[p];
    if (c == '"') return p + 1;
    if (c == '\\') {
      if (p + 1 >= s.size()) return std::string::npos;
      const char esc = s[p + 1];
      switch (esc) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'u':
          // Not decoded — pass through the `\uXXXX` bytes literally so
          // downstream string-match consumers still work. (NIP-01 fields
          // we read are ASCII hex / digits / bolt11, so this is safe.)
          out.push_back('\\');
          out.push_back('u');
          if (p + 5 >= s.size()) return std::string::npos;
          out.push_back(s[p + 2]);
          out.push_back(s[p + 3]);
          out.push_back(s[p + 4]);
          out.push_back(s[p + 5]);
          p += 4;  // consume the 4 hex digits in addition to the `\u`
          break;
        default: return std::string::npos;
      }
      p += 2;
      continue;
    }
    out.push_back(c);
    ++p;
  }
  return std::string::npos;  // unterminated string
}

// Parse a JSON number starting at `p`. On success returns the index
// just past the last digit and sets `out` to the parsed value.
size_t ParseNumber(const std::string& s, size_t p, double& out) {
  const size_t start = p;
  if (p < s.size() && (s[p] == '-' || s[p] == '+')) ++p;
  bool any_digit = false;
  while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
    ++p;
    any_digit = true;
  }
  if (p < s.size() && s[p] == '.') {
    ++p;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9') {
      ++p;
      any_digit = true;
    }
  }
  if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
    ++p;
    if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p;
    while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
  }
  if (!any_digit) return std::string::npos;
  out = std::strtod(s.substr(start, p - start).c_str(), nullptr);
  return p;
}

// Skip any JSON value (object, array, string, number, bool, null).
// Returns the index just past the value, or std::string::npos on error.
size_t SkipValue(const std::string& s, size_t p) {
  p = SkipWs(s, p);
  if (p >= s.size()) return std::string::npos;
  const char c = s[p];
  if (c == '"') {
    std::string tmp;
    return ParseString(s, p, tmp);
  }
  if (c == '{' || c == '[') {
    const char open = c;
    const char close = (c == '{') ? '}' : ']';
    int depth = 0;
    bool in_string = false;
    while (p < s.size()) {
      const char d = s[p];
      if (in_string) {
        if (d == '\\' && p + 1 < s.size()) {
          p += 2;
          continue;
        }
        if (d == '"') in_string = false;
        ++p;
        continue;
      }
      if (d == '"') { in_string = true; ++p; continue; }
      if (d == open) ++depth;
      else if (d == close) {
        --depth;
        if (depth == 0) return p + 1;
      }
      ++p;
    }
    return std::string::npos;
  }
  if (c == '-' || (c >= '0' && c <= '9')) {
    double tmp = 0;
    return ParseNumber(s, p, tmp);
  }
  // true / false / null — scan to next delimiter.
  const size_t start = p;
  while (p < s.size()) {
    const char d = s[p];
    if (d == ',' || d == ']' || d == '}' || d == ' ' || d == '\t' ||
        d == '\n' || d == '\r') {
      break;
    }
    ++p;
  }
  if (p == start) return std::string::npos;
  return p;
}

// Parse an object key, returning the key and the index just past the
// trailing `:`. Assumes `p` points at the opening `"`.
size_t ParseKey(const std::string& s, size_t p, std::string& key) {
  p = ParseString(s, p, key);
  if (p == std::string::npos) return p;
  p = SkipWs(s, p);
  if (p >= s.size() || s[p] != ':') return std::string::npos;
  return SkipWs(s, p + 1);
}

// Parse a tag — a JSON array of strings. `p` must point at the opening `[`.
size_t ParseTag(const std::string& s, size_t p, Tag& out) {
  out.values.clear();
  if (p >= s.size() || s[p] != '[') return std::string::npos;
  ++p;
  p = SkipWs(s, p);
  if (p < s.size() && s[p] == ']') return p + 1;  // empty tag
  while (p < s.size()) {
    if (s[p] != '"') return std::string::npos;
    std::string v;
    p = ParseString(s, p, v);
    if (p == std::string::npos) return p;
    out.values.push_back(std::move(v));
    p = SkipWs(s, p);
    if (p >= s.size()) return std::string::npos;
    if (s[p] == ']') return p + 1;
    if (s[p] != ',') return std::string::npos;
    p = SkipWs(s, p + 1);
  }
  return std::string::npos;
}

// Parse the tags field — an array of Tag arrays.
size_t ParseTags(const std::string& s, size_t p, std::vector<Tag>& out) {
  out.clear();
  if (p >= s.size() || s[p] != '[') return std::string::npos;
  ++p;
  p = SkipWs(s, p);
  if (p < s.size() && s[p] == ']') return p + 1;
  while (p < s.size()) {
    Tag t;
    p = ParseTag(s, p, t);
    if (p == std::string::npos) return p;
    out.push_back(std::move(t));
    p = SkipWs(s, p);
    if (p >= s.size()) return std::string::npos;
    if (s[p] == ']') return p + 1;
    if (s[p] != ',') return std::string::npos;
    p = SkipWs(s, p + 1);
  }
  return std::string::npos;
}

}  // namespace

// --- Event / envelope methods ----------------------------------------

const Tag* Event::FindTag(const std::string& name) const {
  for (const auto& t : tags) {
    if (!t.values.empty() && t.values[0] == name) return &t;
  }
  return nullptr;
}

const std::string& Event::TagValue(const std::string& name) const {
  static const std::string kEmpty;
  const Tag* t = FindTag(name);
  return (t == nullptr) ? kEmpty : t->at(1);
}

// --- Public parsers --------------------------------------------------

bool ParseEventObject(const std::string& json, Event& out) {
  out = Event{};
  size_t p = SkipWs(json, 0);
  if (p >= json.size() || json[p] != '{') return false;
  ++p;
  p = SkipWs(json, p);
  if (p < json.size() && json[p] == '}') return true;

  while (p < json.size()) {
    std::string key;
    p = ParseKey(json, p, key);
    if (p == std::string::npos) return false;

    if (key == "id") {
      p = ParseString(json, p, out.id);
    } else if (key == "pubkey") {
      p = ParseString(json, p, out.pubkey);
    } else if (key == "sig") {
      p = ParseString(json, p, out.sig);
    } else if (key == "content") {
      p = ParseString(json, p, out.content);
    } else if (key == "created_at") {
      double d = 0;
      p = ParseNumber(json, p, d);
      if (p != std::string::npos) out.created_at = static_cast<uint64_t>(d);
    } else if (key == "kind") {
      double d = 0;
      p = ParseNumber(json, p, d);
      if (p != std::string::npos) out.kind = static_cast<uint32_t>(d);
    } else if (key == "tags") {
      p = ParseTags(json, p, out.tags);
    } else {
      // Unknown field — skip value.
      p = SkipValue(json, p);
    }
    if (p == std::string::npos) return false;

    p = SkipWs(json, p);
    if (p >= json.size()) return false;
    if (json[p] == '}') return true;
    if (json[p] != ',') return false;
    p = SkipWs(json, p + 1);
  }
  return false;
}

bool ParseEnvelope(const std::string& frame, Envelope& out) {
  out = Envelope{};
  size_t p = SkipWs(frame, 0);
  if (p >= frame.size() || frame[p] != '[') return false;
  p = SkipWs(frame, p + 1);

  std::string type;
  p = ParseString(frame, p, type);
  if (p == std::string::npos) return false;

  p = SkipWs(frame, p);

  if (type == "EVENT") {
    out.type = EnvelopeType::kEvent;
    if (p >= frame.size() || frame[p] != ',') return false;
    p = SkipWs(frame, p + 1);
    p = ParseString(frame, p, out.sub_id);
    if (p == std::string::npos) return false;
    p = SkipWs(frame, p);
    if (p >= frame.size() || frame[p] != ',') return false;
    p = SkipWs(frame, p + 1);
    // Parse the embedded event object, using a substring bounded by
    // the matching `}`.
    const size_t obj_end = SkipValue(frame, p);
    if (obj_end == std::string::npos) return false;
    if (!ParseEventObject(frame.substr(p, obj_end - p), out.event)) return false;
    return true;
  }
  if (type == "EOSE") {
    out.type = EnvelopeType::kEose;
    if (p >= frame.size() || frame[p] != ',') return false;
    p = SkipWs(frame, p + 1);
    p = ParseString(frame, p, out.sub_id);
    return p != std::string::npos;
  }
  if (type == "CLOSED") {
    out.type = EnvelopeType::kClosed;
    if (p >= frame.size() || frame[p] != ',') return false;
    p = SkipWs(frame, p + 1);
    p = ParseString(frame, p, out.sub_id);
    if (p == std::string::npos) return false;
    p = SkipWs(frame, p);
    if (p < frame.size() && frame[p] == ',') {
      p = SkipWs(frame, p + 1);
      ParseString(frame, p, out.message);
    }
    return true;
  }
  if (type == "NOTICE") {
    out.type = EnvelopeType::kNotice;
    if (p >= frame.size() || frame[p] != ',') return false;
    p = SkipWs(frame, p + 1);
    p = ParseString(frame, p, out.message);
    return p != std::string::npos;
  }
  if (type == "OK") {
    out.type = EnvelopeType::kOk;
    // Shape: ["OK", "<event-id>", <bool>, "<message>"]
    if (p >= frame.size() || frame[p] != ',') return false;
    p = SkipWs(frame, p + 1);
    p = ParseString(frame, p, out.sub_id);  // stash event id in sub_id
    return p != std::string::npos;
  }
  return false;
}

bool ExtractZapAmountMsat(const Event& ev, uint64_t& msat) {
  const Tag* t = ev.FindTag("amount");
  if (t == nullptr) return false;
  const std::string& v = t->at(1);
  if (v.empty()) return false;
  for (char c : v) {
    if (c < '0' || c > '9') return false;
  }
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(v.c_str(), &end, 10);
  if (end != v.c_str() + v.size()) return false;
  msat = parsed;
  return true;
}

bool ExtractZapBolt11(const Event& ev, std::string& bolt11) {
  const Tag* t = ev.FindTag("bolt11");
  if (t == nullptr) return false;
  const std::string& v = t->at(1);
  if (v.empty()) return false;
  bolt11 = v;
  return true;
}

namespace {

// Parse a whole-number decimal string (no sign, no exponent). Returns
// true and writes `out` on success. Leading/trailing whitespace is
// rejected — the publisher emits bare "870124" values.
bool ParseDecimalU32(const std::string& s, uint32_t& out) {
  if (s.empty()) return false;
  uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<uint64_t>(c - '0');
    if (v > 0xFFFFFFFFULL) return false;
  }
  out = static_cast<uint32_t>(v);
  return true;
}

// Parse a decimal number (possibly fractional, e.g. "12.75") into a
// double. Accepts only a leading sign, digits, and at most one '.';
// rejects exponents and whitespace. Stricter than strtod so malformed
// publisher output fails closed rather than coming through as 0.
bool ParseDecimalDouble(const std::string& s, double& out) {
  if (s.empty()) return false;
  size_t i = 0;
  if (s[i] == '+' || s[i] == '-') ++i;
  bool any_digit = false;
  bool seen_dot = false;
  for (; i < s.size(); ++i) {
    const char c = s[i];
    if (c >= '0' && c <= '9') {
      any_digit = true;
    } else if (c == '.' && !seen_dot) {
      seen_dot = true;
    } else {
      return false;
    }
  }
  if (!any_digit) return false;
  char* end = nullptr;
  out = std::strtod(s.c_str(), &end);
  return end == s.c_str() + s.size();
}

}  // namespace

bool ParseNip78Content(const std::string& d_tag, const std::string& content,
                       DataSnapshot& out) {
  if (d_tag == "blockheight") {
    uint32_t h = 0;
    if (!ParseDecimalU32(content, h)) return false;
    out.block_height = h;
    return true;
  }
  if (d_tag == "medianFee") {
    double d = 0;
    if (!ParseDecimalDouble(content, d)) return false;
    out.block_fee_precise = d;
    // Round-half-away-from-zero matches the publisher's upstream
    // integer-fee behaviour (blockfee vs blockfee2 in NOSTR.md).
    out.block_fee = static_cast<int32_t>(d < 0 ? std::ceil(d - 0.5)
                                                : std::floor(d + 0.5));
    return true;
  }
  // Price slot: "price:<CCY>". Currency code is the remainder after the
  // prefix; we don't validate the code (publisher already whitelists it
  // to a known set and new codes auto-provision per NOSTR.md).
  static constexpr const char kPricePrefix[] = "price:";
  constexpr size_t kPricePrefixLen = sizeof(kPricePrefix) - 1;
  if (d_tag.size() > kPricePrefixLen &&
      d_tag.compare(0, kPricePrefixLen, kPricePrefix) == 0) {
    if (content.empty()) return false;
    const std::string ccy = d_tag.substr(kPricePrefixLen);
    if (ccy.empty()) return false;
    out.prices[ccy] = content;
    return true;
  }
  return false;
}

}  // namespace nostr
}  // namespace btclock
