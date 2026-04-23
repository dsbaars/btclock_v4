// Host tests for the frontlight fader interpolator.
//
// Only pure-logic cases — FrontlightFader has no FreeRTOS / ESP-IDF
// dependency so we include + link it directly. See btclock_v3_fci-7ma.

#include "doctest.h"

#include <cstdint>

#include "app/frontlight_fader.hpp"

using btclock::FrontlightFader;

TEST_CASE("fade from 0 to max reaches target at exactly ceil(max/step) ticks") {
  // Full-travel invariant pinned at ceil(max/step), matching the old
  // firmware's fade loop (led_handler.cpp:598). For max=2048, step=25
  // that's ceil(2048/25) = 82.
  constexpr uint16_t kMax = 2048;
  constexpr uint16_t kStep = 25;
  constexpr int kExpectedTicks = (kMax + kStep - 1) / kStep;  // 82

  FrontlightFader f(kMax, kStep);
  f.SetTarget(kMax);
  CHECK(f.current() == 0);

  int ticks = 0;
  while (!f.AtTarget()) {
    f.Step();
    ++ticks;
    REQUIRE(ticks <= kExpectedTicks);  // guards against infinite loop
  }
  CHECK(ticks == kExpectedTicks);
  CHECK(f.current() == kMax);

  // Idempotent at target.
  CHECK(f.Step() == kMax);
  CHECK(f.Step() == kMax);
}

TEST_CASE("mid-fade target change redirects smoothly without jump") {
  // Start at 0, aim for max. Step halfway, then redirect to a new
  // target below current. The fader must move from its actual current
  // value toward the new target — no teleport to the new target and
  // no overshoot past max.
  constexpr uint16_t kMax = 2048;
  constexpr uint16_t kStep = 25;
  FrontlightFader f(kMax, kStep);
  f.SetTarget(kMax);

  // Drive 30 ticks: current = 30 * 25 = 750.
  for (int i = 0; i < 30; ++i) f.Step();
  CHECK(f.current() == 750);

  // Redirect to 100. Current should decrement, not jump.
  f.SetTarget(100);
  CHECK(f.current() == 750);  // no immediate jump
  CHECK(f.target() == 100);

  const uint16_t after_one = f.Step();
  CHECK(after_one == 725);  // 750 - 25, one step down

  // Finish the descent; must land exactly on 100 (not overshoot to
  // 99 or 75). (750 - 100) / 25 = 26, and we've taken 1 above, so 25
  // more steps.
  for (int i = 0; i < 25; ++i) f.Step();
  CHECK(f.current() == 100);
  CHECK(f.AtTarget());
}

TEST_CASE("negative target clamps to 0, over-max clamps to max") {
  constexpr uint16_t kMax = 2048;
  constexpr uint16_t kStep = 25;
  FrontlightFader f(kMax, kStep);

  f.SetTarget(-500);
  CHECK(f.target() == 0);

  f.SetTarget(99999);
  CHECK(f.target() == kMax);

  // And a Snap with a negative value lands at 0, not wrapping around.
  f.Snap(-1);
  CHECK(f.current() == 0);
  CHECK(f.target() == 0);
}
