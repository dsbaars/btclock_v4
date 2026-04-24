// Host tests for the WiFi long-outage soft-watchdog decision helper.
// Covers the edge cases the reboot path depends on: disabled watchdog,
// below/above threshold, and the "never disconnected" sentinel.

#include "doctest.h"

#include <cstdint>

#include "io/wifi_guard.hpp"

TEST_CASE("ShouldOutageReboot: zero outage_minutes disables the watchdog") {
  // Even hours of disconnect must not reboot when the user sets 0.
  CHECK_FALSE(btclock::ShouldOutageReboot(1'000u, 10'000'000u, 0u));
}

TEST_CASE("ShouldOutageReboot: disconnected_since_ms == 0 means not armed") {
  // Sentinel for "currently connected" — Tick clears the stamp on every
  // connected tick, and we must not interpret 0 as "disconnected at boot".
  CHECK_FALSE(btclock::ShouldOutageReboot(0u, 10'000'000u, 10u));
}

TEST_CASE("ShouldOutageReboot: below threshold returns false") {
  // 5 min elapsed, 10 min threshold → keep waiting.
  const uint32_t start = 1'000u;
  const uint32_t now = start + 5u * 60u * 1000u;
  CHECK_FALSE(btclock::ShouldOutageReboot(start, now, 10u));
}

TEST_CASE("ShouldOutageReboot: exactly at threshold returns true") {
  // Boundary: >= semantics. 10-min threshold trips on the exact tick.
  const uint32_t start = 1'000u;
  const uint32_t now = start + 10u * 60u * 1000u;
  CHECK(btclock::ShouldOutageReboot(start, now, 10u));
}

TEST_CASE("ShouldOutageReboot: past threshold returns true") {
  // 15 min elapsed, 10 min threshold → reboot.
  const uint32_t start = 1'000u;
  const uint32_t now = start + 15u * 60u * 1000u;
  CHECK(btclock::ShouldOutageReboot(start, now, 10u));
}

TEST_CASE("ShouldOutageReboot: max setting (120 min) honoured") {
  // Upper range of the schema clamp. One second before, not yet; right
  // at 120 min, reboot.
  const uint32_t start = 500u;
  const uint32_t one_before = start + 120u * 60u * 1000u - 1u;
  const uint32_t at = start + 120u * 60u * 1000u;
  CHECK_FALSE(btclock::ShouldOutageReboot(start, one_before, 120u));
  CHECK(btclock::ShouldOutageReboot(start, at, 120u));
}
