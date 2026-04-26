// Pure-logic implementation for the HTTP Basic auth gate. Must stay
// free of ESP-IDF headers so the host tests can link it without
// pulling in the firmware toolchain.

#include "auth_gate_logic.hpp"

#include <cstdint>

namespace btclock {
namespace auth {
namespace {

// Base64 decode table. -1 for invalid bytes; -2 is reserved for the `=`
// pad character so the parser can special-case it separately from
// whitespace/garbage.
constexpr int kPad = -2;
constexpr int kBad = -1;

constexpr int kB64Table[256] = {
    // 0..31 — all control chars are rejected (no whitespace tolerance).
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    // 32..63 — '+' = 62, '/' = 63, '0'..'9' = 52..61, '=' = pad.
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    62,
    kBad,
    kBad,
    kBad,
    63,
    52,
    53,
    54,
    55,
    56,
    57,
    58,
    59,
    60,
    61,
    kBad,
    kBad,
    kBad,
    kPad,
    kBad,
    kBad,
    // 64..95 — 'A'..'Z' = 0..25.
    kBad,
    0,
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    9,
    10,
    11,
    12,
    13,
    14,
    15,
    16,
    17,
    18,
    19,
    20,
    21,
    22,
    23,
    24,
    25,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    // 96..127 — 'a'..'z' = 26..51.
    kBad,
    26,
    27,
    28,
    29,
    30,
    31,
    32,
    33,
    34,
    35,
    36,
    37,
    38,
    39,
    40,
    41,
    42,
    43,
    44,
    45,
    46,
    47,
    48,
    49,
    50,
    51,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    // 128..255 — all invalid.
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
    kBad,
};

char AsciiLower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

}  // namespace

bool DecodeBase64(std::string_view src, std::string* out) {
  out->clear();
  // Length must be a multiple of 4 for canonical base64. HTTP Basic
  // generators always pad, so rejecting un-padded inputs is the safe
  // choice — it turns "Basic abc" (attacker typo) into a clean 401
  // instead of a partial decode.
  if (src.size() % 4 != 0) return false;

  out->reserve(src.size() / 4 * 3);
  for (size_t i = 0; i < src.size(); i += 4) {
    const int c0 = kB64Table[static_cast<uint8_t>(src[i])];
    const int c1 = kB64Table[static_cast<uint8_t>(src[i + 1])];
    const int c2 = kB64Table[static_cast<uint8_t>(src[i + 2])];
    const int c3 = kB64Table[static_cast<uint8_t>(src[i + 3])];
    // First two characters can never be pad — a leading/second pad
    // position would mean the group encodes zero data bytes, which
    // is malformed.
    if (c0 < 0 || c1 < 0 || c0 == kPad || c1 == kPad) return false;

    const uint8_t b0 = static_cast<uint8_t>((c0 << 2) | (c1 >> 4));
    out->push_back(static_cast<char>(b0));
    if (c2 == kPad) {
      // "xx==" — one output byte. c3 must also be pad, and this must
      // be the last quad.
      if (c3 != kPad) return false;
      if (i + 4 != src.size()) return false;
      break;
    }
    if (c2 < 0) return false;
    const uint8_t b1 = static_cast<uint8_t>(((c1 & 0x0F) << 4) | (c2 >> 2));
    out->push_back(static_cast<char>(b1));
    if (c3 == kPad) {
      if (i + 4 != src.size()) return false;
      break;
    }
    if (c3 < 0) return false;
    const uint8_t b2 = static_cast<uint8_t>(((c2 & 0x03) << 6) | c3);
    out->push_back(static_cast<char>(b2));
  }
  return true;
}

bool ParseUserPass(std::string_view decoded, std::string* user,
                   std::string* pass) {
  const size_t colon = decoded.find(':');
  if (colon == std::string_view::npos) {
    user->clear();
    pass->clear();
    return false;
  }
  user->assign(decoded.substr(0, colon));
  pass->assign(decoded.substr(colon + 1));
  return true;
}

bool ExtractBasicToken(std::string_view value, std::string_view* out) {
  // "Basic " is 6 chars. Scheme compare is ASCII case-insensitive.
  if (value.size() < 6) return false;
  const char* scheme = "basic ";
  for (size_t i = 0; i < 6; ++i) {
    if (AsciiLower(value[i]) != scheme[i]) return false;
  }
  // Skip any extra spaces between scheme and token. RFC 7235 allows
  // one or more `SP` between the scheme and the credentials; common
  // clients emit exactly one, but being liberal here is cheap.
  size_t start = 6;
  while (start < value.size() && value[start] == ' ') ++start;
  *out = value.substr(start);
  return !out->empty();
}

bool ConstantTimeEquals(std::string_view a, std::string_view b) {
  // Walk max(len_a, len_b) so a length mismatch doesn't exit early.
  // Using unsigned char avoids signed-shift UB on compilers that are
  // picky about xor of mixed-signedness bytes.
  const size_t n = a.size() > b.size() ? a.size() : b.size();
  unsigned int diff = (a.size() == b.size()) ? 0u : 1u;
  for (size_t i = 0; i < n; ++i) {
    const unsigned char ca =
        i < a.size() ? static_cast<unsigned char>(a[i]) : 0u;
    const unsigned char cb =
        i < b.size() ? static_cast<unsigned char>(b[i]) : 0u;
    diff |= static_cast<unsigned int>(ca ^ cb);
  }
  return diff == 0u;
}

bool CredentialsMatch(std::string_view supplied_user,
                      std::string_view supplied_pass,
                      std::string_view configured_user,
                      std::string_view configured_pass) {
  // Evaluate *both* sides unconditionally with `|` (not `&&`) so the
  // total comparison time depends only on the longer configured
  // string, never on whether the username already failed.
  const bool u = ConstantTimeEquals(supplied_user, configured_user);
  const bool p = ConstantTimeEquals(supplied_pass, configured_pass);
  return u && p;
}

}  // namespace auth
}  // namespace btclock
