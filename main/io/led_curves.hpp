// Pure-logic helpers for NeoPixel effect curves.
//
// Split out from led_controller.hpp so the math can be covered by host
// tests (test_host/ cannot link FreeRTOS / RMT code). Matches the feel
// of the old Arduino firmware's effects:
//
//   - Heartbeat breath: slow sine-shaped pulse over ~2 s.
//   - Data-error breath: same shape but red and slower (alive but sick).
//   - Generic ramp: linear 0..max over N ticks.
//
// The "color parse" helper accepts WebUI-supplied hex strings
// (`"#RRGGBB"` or `"RRGGBB"`) and returns a packed 0x00RRGGBB uint32
// suitable for NVS persistence and for splitting into (r, g, b) bytes
// at the call site. Invalid inputs return the caller-supplied fallback.
//
// All functions here are header-inline-able and have no external
// dependencies beyond <cstdint>, so they work in both the ESP-IDF build
// and the host-test harness unchanged.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace btclock {
namespace led_curves {

// Linear ramp from 0 to `peak` over `total_ticks` ticks. The final
// tick (total_ticks - 1) is the peak; anything beyond clamps to peak
// so callers that hand out the last frame don't see a sudden drop
// to 0. `total_ticks` must be > 0 — degenerate 0 returns 0.
inline uint8_t Ramp(uint8_t peak, uint32_t tick, uint32_t total_ticks) {
  if (total_ticks == 0) return 0;
  if (total_ticks == 1) return peak;  // degenerate single-tick ramp
  if (tick >= total_ticks - 1) return peak;
  // Integer scale — avoids FPU and matches the 8-bit NeoPixel colour
  // format naturally.
  return static_cast<uint8_t>((static_cast<uint32_t>(peak) * tick) /
                              (total_ticks - 1));
}

// Sine-shaped breath curve. Returns an intensity in [0, peak] following
// a single breath (up then down) over `total_ticks` ticks.
//
// Uses a cheap 4-term polynomial approximation of (1 - cos(2*pi*t)) / 2
// so we don't drag in <cmath>. The shape is symmetric: intensity(0) = 0,
// intensity(total_ticks/2) = peak, intensity(total_ticks) = 0.
//
// `total_ticks` must be > 0; `tick` is clamped into [0, total_ticks).
uint8_t Breath(uint8_t peak, uint32_t tick, uint32_t total_ticks);

// Parse a 24-bit RGB color from `"#RRGGBB"` / `"RRGGBB"` / `"#rgb"` /
// `"rgb"`. Returns `fallback` for any parse failure (length, non-hex,
// etc.). Packed as 0x00RRGGBB to match the old firmware's
// `DEFAULT_BLOCK_FLASH_COLOR` storage format (e.g. 0xE04300).
uint32_t ParseHexColor(std::string_view in, uint32_t fallback);

// Format a 24-bit RGB color as `"#RRGGBB"` (uppercase). Always writes 8
// bytes (7 chars + NUL). `out` must point to at least 8 bytes of
// writable storage. Returns the byte count excluding the terminator.
size_t FormatHexColor(uint32_t rgb, char* out);

// Scale an 8-bit per-channel color by an 8-bit brightness. brightness=0
// blacks out; brightness=255 is identity. Matches Adafruit_NeoPixel's
// setBrightness scaling (linear, not gamma-corrected). Wraps the common
// "multiply-divide-by-255" pattern so handlers don't repeat it.
inline uint8_t Scale(uint8_t channel, uint8_t brightness) {
  return static_cast<uint8_t>((static_cast<uint16_t>(channel) * brightness) /
                              255);
}

}  // namespace led_curves
}  // namespace btclock
