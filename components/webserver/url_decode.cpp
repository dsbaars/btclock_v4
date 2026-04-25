#include "url_decode.hpp"

#include <cstddef>

namespace btclock {
namespace http {
namespace {

// -1 means "not a hex digit" so the caller can detect malformed input
// without a second pass.
int HexValue(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

bool UrlDecodeInPlace(char* buf) {
  if (buf == nullptr) return false;
  std::size_t w = 0;
  for (std::size_t r = 0; buf[r] != '\0'; ++r) {
    const char c = buf[r];
    if (c == '+') {
      buf[w++] = ' ';
      continue;
    }
    if (c == '%') {
      const char a = buf[r + 1];
      if (a == '\0') return false;
      const char b = buf[r + 2];
      if (b == '\0') return false;
      const int hi = HexValue(a);
      const int lo = HexValue(b);
      if (hi < 0 || lo < 0) return false;
      buf[w++] = static_cast<char>((hi << 4) | lo);
      r += 2;
      continue;
    }
    buf[w++] = c;
  }
  buf[w] = '\0';
  return true;
}

}  // namespace http
}  // namespace btclock
