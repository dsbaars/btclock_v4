// Host tests for the network LED fault-indicator decision helper.
// Covers the grace debounce that keeps a brief WiFi blip (or a momentary
// double-feed stall) from flashing the alarming red breath, while leaving
// the quieter per-source tiers immediate. The stateful Tick() itself
// pulls in the LED controller + ESP-IDF and isn't host-linkable; the pure
// ShouldPostIndicator() carries the logic worth pinning.

#include <cstdint>

#include "doctest.h"
#include "io/network_led_watchdog.hpp"

using btclock::kRedBreathGraceMs;
using btclock::ShouldPostIndicator;

namespace {
// Cadences from CadenceMs(): red tiers 5 s, quiet per-source tiers 10 s.
constexpr uint32_t kGrace = kRedBreathGraceMs;  // 5 s
constexpr uint32_t kRedCadence = 5'000u;
constexpr uint32_t kQuietCadence = 10'000u;
}  // namespace

TEST_CASE("ShouldPostIndicator: chosen grace window is 5 s") {
  // Pin the configured value — the whole point is to clear a clean STA
  // auto-reconnect (~2 s retry + association/DHCP) before painting red.
  CHECK(kRedBreathGraceMs == 5'000u);
}

TEST_CASE("ShouldPostIndicator: quiet tier posts immediately on tier change") {
  // kBlock / kPrice are low-urgency and seldom transient — no debounce.
  CHECK(ShouldPostIndicator(/*is_red=*/false, /*tier_changed=*/true,
                            /*now_ms=*/1'000u, /*red_since_ms=*/0u,
                            /*last_post_ms=*/0u, kGrace, kQuietCadence));
}

TEST_CASE("ShouldPostIndicator: quiet tier re-posts on cadence, not before") {
  // last post at 1 s, 10 s cadence: silent at 5 s, fires at 11 s.
  CHECK_FALSE(ShouldPostIndicator(false, false, 5'000u, 0u, 1'000u, kGrace,
                                  kQuietCadence));
  CHECK(ShouldPostIndicator(false, false, 11'000u, 0u, 1'000u, kGrace,
                            kQuietCadence));
}

TEST_CASE(
    "ShouldPostIndicator: red breath stays silent within the grace window") {
  // Episode opens at 1 s. The leading-edge tick and every tick inside the
  // window must NOT paint — this is the regression the user reported.
  CHECK_FALSE(ShouldPostIndicator(true, /*tier_changed=*/true, 1'000u, 1'000u,
                                  /*last_post_ms=*/0u, kGrace, kRedCadence));
  CHECK_FALSE(ShouldPostIndicator(true, false, 3'000u, 1'000u, 0u, kGrace,
                                  kRedCadence));
  CHECK_FALSE(ShouldPostIndicator(true, false, 5'999u, 1'000u, 0u, kGrace,
                                  kRedCadence));
}

TEST_CASE("ShouldPostIndicator: red breath fires exactly at grace expiry") {
  // >= semantics: 1 s + 5 s grace trips at 6 s, not 5.999 s. last_post=0
  // (predates this episode) so it's the first breath of the episode.
  CHECK(ShouldPostIndicator(true, false, 6'000u, 1'000u, 0u, kGrace,
                            kRedCadence));
}

TEST_CASE("ShouldPostIndicator: a brief blip never paints red") {
  // WiFi drops at 1 s, auto-reconnects ~3 s later. Every tick the fault is
  // visible falls inside the grace window, so no red breath is ever posted
  // — the caller then sees kNone and the resting paint just stands.
  for (uint32_t now = 1'000u; now <= 4'000u; now += 1'000u) {
    CHECK_FALSE(ShouldPostIndicator(true, now == 1'000u, now, 1'000u, 0u,
                                    kGrace, kRedCadence));
  }
}

TEST_CASE(
    "ShouldPostIndicator: red breath re-posts on cadence after first fire") {
  // First breath posted at 6 s. Next re-post is one 5 s cadence later.
  CHECK_FALSE(ShouldPostIndicator(true, false, 9'000u, 1'000u,
                                  /*last_post=*/6'000u, kGrace, kRedCadence));
  CHECK(ShouldPostIndicator(true, false, 11'000u, 1'000u, 6'000u, kGrace,
                            kRedCadence));
}

TEST_CASE(
    "ShouldPostIndicator: kWifi<->kMulti hand-off does not restart grace") {
  // Episode began at 1 s and first painted at 6 s. WiFi returns at 8 s but
  // both feeds are still cold → kMulti (tier_changed=true, still red),
  // red_since unchanged at 1 s. The hand-off must NOT double-post; it
  // stays on the running cadence (2 s since the last post < 5 s).
  CHECK_FALSE(ShouldPostIndicator(true, /*tier_changed=*/true, 8'000u, 1'000u,
                                  /*last_post=*/6'000u, kGrace, kRedCadence));
  // ...and the next cadence tick at 11 s does fire.
  CHECK(ShouldPostIndicator(true, false, 11'000u, 1'000u, 6'000u, kGrace,
                            kRedCadence));
}
