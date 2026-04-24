// Host tests for the NeoPixel effect curve helpers.
//
// Pure-logic only — see main/app/led_curves.hpp for the contract.
// Exercises the Breath + Ramp + Scale helpers that the FreeRTOS-side
// effect handlers rely on (heartbeat, data-error, identify decay).

#include "doctest.h"

#include <cstdint>

#include "io/led_curves.hpp"

using btclock::led_curves::Breath;
using btclock::led_curves::Ramp;
using btclock::led_curves::Scale;

TEST_CASE("Ramp(total_ticks == 0) returns 0 regardless of peak") {
  // Degenerate input — handlers that call Ramp with 0 ticks should
  // fall out of the loop before they call it, but the helper is
  // defensive: zero ticks means zero intensity.
  CHECK(Ramp(255, 0, 0) == 0);
  CHECK(Ramp(128, 5, 0) == 0);
}

TEST_CASE("Ramp returns peak at the final tick and beyond") {
  // Final tick: saturates at peak. Callers commonly hand out the
  // "final frame" so this is the hot path.
  CHECK(Ramp(200, 9, 10) == 200);
  CHECK(Ramp(200, 10, 10) == 200);
  CHECK(Ramp(200, 100, 10) == 200);  // clamped
}

TEST_CASE("Ramp is monotonic non-decreasing over the full interval") {
  // Any curve that rises must never dip — otherwise the LED flickers.
  constexpr uint8_t kPeak = 255;
  constexpr uint32_t kTotal = 32;
  uint8_t last = 0;
  for (uint32_t t = 0; t <= kTotal; ++t) {
    const uint8_t v = Ramp(kPeak, t, kTotal);
    CHECK(v >= last);
    last = v;
  }
}

TEST_CASE("Breath starts and ends at zero, peaks near the midpoint") {
  // Invariant pinned: Breath(0) == 0 and Breath(total-1) approaches 0.
  // Peak (within 1 unit of `peak`) should land at roughly total/2.
  constexpr uint8_t kPeak = 200;
  constexpr uint32_t kTotal = 60;

  CHECK(Breath(kPeak, 0, kTotal) == 0);
  // Last tick — may be small but must be << peak.
  CHECK(Breath(kPeak, kTotal - 1, kTotal) <= 10);

  // Find max over the interval; expect it to be close to peak and to
  // occur within a reasonable window around the midpoint.
  uint8_t max_v = 0;
  uint32_t max_tick = 0;
  for (uint32_t t = 0; t < kTotal; ++t) {
    const uint8_t v = Breath(kPeak, t, kTotal);
    if (v > max_v) {
      max_v = v;
      max_tick = t;
    }
  }
  CHECK(max_v >= kPeak - 2);  // peak reached modulo integer rounding
  CHECK(max_tick >= kTotal / 2 - 2);
  CHECK(max_tick <= kTotal / 2 + 2);
}

TEST_CASE("Breath curve is symmetric around the midpoint") {
  // Symmetry lets the handler drive a clean breath without a
  // "direction flip" at the peak. Matches the shape of the old
  // firmware's blink-then-fade pattern.
  constexpr uint8_t kPeak = 255;
  constexpr uint32_t kTotal = 40;
  for (uint32_t t = 0; t < kTotal / 2; ++t) {
    const uint8_t left = Breath(kPeak, t, kTotal);
    const uint8_t right = Breath(kPeak, kTotal - 1 - t, kTotal);
    // Allow 1 unit of rounding slack.
    const int delta = static_cast<int>(left) - static_cast<int>(right);
    CHECK(delta >= -1);
    CHECK(delta <= 1);
  }
}

TEST_CASE("Scale applies linear 8-bit brightness without overflow") {
  // brightness=255 is identity; 0 is mute; 128 is ~half.
  CHECK(Scale(0, 255) == 0);
  CHECK(Scale(255, 255) == 255);
  CHECK(Scale(255, 0) == 0);
  CHECK(Scale(200, 128) == 100);  // (200 * 128) / 255 = 100
  CHECK(Scale(255, 128) == 128);  // (255 * 128) / 255 = 128
  // No overflow despite the intermediate > 255.
  CHECK(Scale(255, 254) == 254);
}
