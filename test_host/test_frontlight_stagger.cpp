// Host tests for the staggered frontlight flash animation math.
//
// Pins the v3 visual signature: LED index 0 leads on kIn, lags on kOut,
// and the full animation completes in a deterministic tick count given
// (max_brightness, step, num_leds). See frontlight_logic/frontlight_stagger.hpp for
// the contract; see v3_fci src/lib/drivers/leds/led_handler.cpp:573-646
// for the original loops this ports.

#include <cstdint>

#include "doctest.h"
#include "frontlight_logic/frontlight_stagger.hpp"

using btclock::ComputeStaggeredDuty;
using btclock::StaggerDelayMs;
using btclock::StaggerDirection;
using btclock::StaggerTotalTicks;

TEST_CASE("kIn tick 0: only LED 0 is (barely) lit; others still dark") {
  // The first pulse of fade-in lights LED 0 first. At tick 0 LED 0
  // gets `0 * step - 0` = 0 brightness — v3's loop did the write
  // before the step, so the visible first frame is all dark. We
  // preserve that.
  constexpr uint16_t kMax = 2048;
  constexpr uint16_t kStep = 25;
  constexpr uint8_t kN = 7;

  for (uint8_t i = 0; i < kN; ++i) {
    CHECK(ComputeStaggeredDuty(0, i, kN, kMax, kStep, StaggerDirection::kIn) ==
          0);
  }
}

TEST_CASE("kIn: later LEDs stay dark until the leader reaches their phase") {
  // phase = max / N. LED i starts ramping when tick * step >= i * phase.
  constexpr uint16_t kMax = 2100;  // divides evenly by N=7 -> phase=300
  constexpr uint16_t kStep = 100;
  constexpr uint8_t kN = 7;
  constexpr uint16_t kPhase = kMax / kN;  // 300

  // At tick 1, leader (index 0) = 100; index 1 wants 100 - 300 = -200
  // => clamped to 0. Indices 2..6 also clamped to 0.
  CHECK(ComputeStaggeredDuty(1, 0, kN, kMax, kStep, StaggerDirection::kIn) ==
        100);
  for (uint8_t i = 1; i < kN; ++i) {
    CHECK(ComputeStaggeredDuty(1, i, kN, kMax, kStep, StaggerDirection::kIn) ==
          0);
  }

  // At tick 3, leader = 300 >= phase -> LED 1 lights at 0 (just
  // started). LED 0 = 300, LED 2..6 still 0.
  CHECK(ComputeStaggeredDuty(3, 0, kN, kMax, kStep, StaggerDirection::kIn) ==
        300);
  CHECK(ComputeStaggeredDuty(3, 1, kN, kMax, kStep, StaggerDirection::kIn) ==
        0);
  CHECK(ComputeStaggeredDuty(3, 2, kN, kMax, kStep, StaggerDirection::kIn) ==
        0);
  CHECK(kPhase == 300);  // silence unused warning + doc the phase
}

TEST_CASE("kIn completes with every LED at max on the last tick") {
  constexpr uint16_t kMax = 2048;
  constexpr uint16_t kStep = 25;
  constexpr uint8_t kN = 7;
  const uint32_t total = StaggerTotalTicks(kN, kMax, kStep);

  // On the final tick every LED must be saturated at max.
  for (uint8_t i = 0; i < kN; ++i) {
    CHECK(ComputeStaggeredDuty(total - 1, i, kN, kMax, kStep,
                               StaggerDirection::kIn) == kMax);
  }
}

TEST_CASE("kOut completes with every LED at 0 on the last tick") {
  constexpr uint16_t kMax = 2048;
  constexpr uint16_t kStep = 25;
  constexpr uint8_t kN = 7;
  const uint32_t total = StaggerTotalTicks(kN, kMax, kStep);

  for (uint8_t i = 0; i < kN; ++i) {
    CHECK(ComputeStaggeredDuty(total - 1, i, kN, kMax, kStep,
                               StaggerDirection::kOut) == 0);
  }
}

TEST_CASE("kOut tick 0: LED 0 already partially dim, LED N-1 at max") {
  // v3 quirk we preserve (see led_handler.cpp:624): at tick 0 of
  // fadeOutAll the outer dutyCycle is max, and the per-LED offset is
  // (N-1-i) * max/N. So LED 0 starts at max - (N-1)*max/N ≈ max/N
  // (small), LED N-1 starts at max - 0 = max. This is what gives the
  // cascade its 0 -> N-1 direction on the darkening half: LED 0 has
  // the least room to fall, so it reaches 0 first.
  constexpr uint16_t kMax = 2100;  // divides evenly: phase=300
  constexpr uint16_t kStep = 25;
  constexpr uint8_t kN = 7;

  // LED 0 at tick 0 = 2100 - 6*300 = 300
  CHECK(ComputeStaggeredDuty(0, 0, kN, kMax, kStep, StaggerDirection::kOut) ==
        300);
  // LED N-1 at tick 0 = 2100 - 0 = 2100
  CHECK(ComputeStaggeredDuty(0, kN - 1, kN, kMax, kStep,
                             StaggerDirection::kOut) == kMax);

  // LED 0 hits zero first on kOut: at ceil(300/25) = 12 ticks in.
  CHECK(ComputeStaggeredDuty(12, 0, kN, kMax, kStep, StaggerDirection::kOut) ==
        0);
  // LED N-1 still bright at that tick (2100 - 12*25 = 1800).
  CHECK(ComputeStaggeredDuty(12, kN - 1, kN, kMax, kStep,
                             StaggerDirection::kOut) == 1800);
}

TEST_CASE("StaggerDelayMs floors at 1 and divides by LED count") {
  // flEffectDelay=15, N=7 -> floor(15/7) = 2ms.
  CHECK(StaggerDelayMs(15, 7) == 2);
  // flEffectDelay=1, N=7 -> floor(1/7) = 0 -> clamp to 1.
  CHECK(StaggerDelayMs(1, 7) == 1);
  // flEffectDelay=0, N=anything -> clamp to 1.
  CHECK(StaggerDelayMs(0, 7) == 1);
  // Exactly divisible: 14/7 = 2.
  CHECK(StaggerDelayMs(14, 7) == 2);
  // Zero LEDs — guard returns the input untouched (never exercised
  // in practice but the function must not divide by zero).
  CHECK(StaggerDelayMs(15, 0) == 15);
}

TEST_CASE("out-of-range led_index returns 0 rather than UB") {
  constexpr uint16_t kMax = 2048;
  constexpr uint16_t kStep = 25;
  constexpr uint8_t kN = 7;
  CHECK(ComputeStaggeredDuty(10, kN, kN, kMax, kStep, StaggerDirection::kIn) ==
        0);
  CHECK(ComputeStaggeredDuty(10, 250, kN, kMax, kStep,
                             StaggerDirection::kOut) == 0);
}

TEST_CASE("total ticks matches v3 loop bound for canonical params") {
  // v3 Rev-B defaults: max=2048, step=25, N=7.
  //   bound = 2048 + 6 * (2048/7) = 2048 + 6*292 = 3800
  //   iterations = 3800/25 + 1 = 152 + 1 = 153
  // Pin this — if someone retunes kFadeStep or the stagger formula
  // the flash timing will shift and users will notice.
  CHECK(StaggerTotalTicks(7, 2048, 25) == 153);
}
