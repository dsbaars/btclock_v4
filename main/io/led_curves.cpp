#include "io/led_curves.hpp"

#include <cctype>
#include <cstdio>

namespace btclock {
namespace led_curves {
namespace {

// Polynomial approximation of (1 - cos(2*pi*x)) / 2 over x in [0, 1].
// Derived empirically to match the breath feel of the old firmware
// without dragging in <cmath>. Max absolute error vs. the true curve
// is ~0.02 — imperceptible at 8-bit LED intensity.
//
// Piecewise to preserve symmetry: mirror around x = 0.5.
float BreathNormalized(float x) {
  if (x < 0.0f) x = 0.0f;
  if (x > 1.0f) x = 1.0f;
  // Mirror: both halves rise from 0 to 1 linearly-then-flatten.
  const float t = (x < 0.5f) ? (x * 2.0f) : ((1.0f - x) * 2.0f);
  // Smoothstep-style 3t^2 - 2t^3 gives a soft, cos-like rise/fall.
  return t * t * (3.0f - 2.0f * t);
}

int HexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

uint8_t Breath(uint8_t peak, uint32_t tick, uint32_t total_ticks) {
  if (total_ticks == 0) return 0;
  if (tick >= total_ticks) tick = total_ticks - 1;
  // Use (total_ticks - 1) as the denominator so x spans [0, 1] inclusive
  // (not [0, 1) with a dropped endpoint). This keeps the breath curve
  // symmetric around the midpoint — tick t and tick (total - 1 - t) map
  // to mirrored x values, ensuring Breath(t) == Breath(total - 1 - t)
  // to within rounding.
  const float denom =
      total_ticks > 1 ? static_cast<float>(total_ticks - 1) : 1.0f;
  const float x = static_cast<float>(tick) / denom;
  const float y = BreathNormalized(x);
  const uint32_t scaled = static_cast<uint32_t>(
      (y * static_cast<float>(peak)) + 0.5f);
  if (scaled > peak) return peak;
  return static_cast<uint8_t>(scaled);
}

uint32_t ParseHexColor(std::string_view in, uint32_t fallback) {
  if (!in.empty() && in.front() == '#') in.remove_prefix(1);
  // Accept both 3-digit shorthand (#rgb -> #rrggbb) and 6-digit.
  if (in.size() == 3) {
    const int r = HexDigit(in[0]);
    const int g = HexDigit(in[1]);
    const int b = HexDigit(in[2]);
    if (r < 0 || g < 0 || b < 0) return fallback;
    const uint32_t rr = static_cast<uint32_t>((r << 4) | r);
    const uint32_t gg = static_cast<uint32_t>((g << 4) | g);
    const uint32_t bb = static_cast<uint32_t>((b << 4) | b);
    return (rr << 16) | (gg << 8) | bb;
  }
  if (in.size() != 6) return fallback;
  uint32_t acc = 0;
  for (size_t i = 0; i < 6; ++i) {
    const int d = HexDigit(in[i]);
    if (d < 0) return fallback;
    acc = (acc << 4) | static_cast<uint32_t>(d);
  }
  return acc & 0x00FFFFFFu;
}

size_t FormatHexColor(uint32_t rgb, char* out) {
  // snprintf returns the number of chars that *would* be written (excl.
  // NUL), which for our fixed format is always 7.
  const int n = std::snprintf(out, 8, "#%06X",
                              static_cast<unsigned>(rgb & 0x00FFFFFFu));
  return n < 0 ? 0 : static_cast<size_t>(n);
}

}  // namespace led_curves
}  // namespace btclock
