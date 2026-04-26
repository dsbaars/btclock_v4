// Host tests for the auto-rotation pause + deadline-reset primitive
// that ScreenManager::MaybeAutoRotate is built on.
//
// ScreenManager itself pulls in the EPD driver, fonts bundle, and
// FreeRTOS (via esp_log) — all unsuitable for the host test target.
// The rotation-timer state and decision live in app/rotation_timer.hpp
// so the pause / restart / manual-advance semantics can be pinned
// here in pure C++.

#include "app/rotation_timer.hpp"
#include "doctest.h"

using btclock::RotationTimer;

TEST_CASE("ShouldAdvance is false before the deadline elapses") {
  RotationTimer t;
  t.last_change_ms = 1'000;
  // 30 s period, probed 20 s in.
  CHECK_FALSE(t.ShouldAdvance(21'000, 30'000));
}

TEST_CASE("ShouldAdvance is true once the deadline elapses") {
  RotationTimer t;
  t.last_change_ms = 1'000;
  CHECK(t.ShouldAdvance(31'000, 30'000));
  // Also true well beyond the deadline.
  CHECK(t.ShouldAdvance(60'000, 30'000));
}

TEST_CASE("ShouldAdvance returns false while paused, even past the deadline") {
  RotationTimer t;
  t.last_change_ms = 0;
  t.paused = true;
  // Well past any reasonable period — pause always wins.
  CHECK_FALSE(t.ShouldAdvance(60'000, 30'000));
  CHECK_FALSE(t.ShouldAdvance(1'000'000'000, 30'000));
}

TEST_CASE("Restart zeroes the deadline from the supplied tick") {
  RotationTimer t;
  t.last_change_ms = 0;
  // Deadline met at t=40s for a 30s period.
  CHECK(t.ShouldAdvance(40'000, 30'000));
  // After Restart, we need another 30s from now — probe 25s in
  // returns false.
  t.Restart(40'000);
  CHECK_FALSE(t.ShouldAdvance(65'000, 30'000));
  // 30s after the restart, we're due again.
  CHECK(t.ShouldAdvance(70'000, 30'000));
}

TEST_CASE("Restart preserves pause state") {
  RotationTimer t;
  t.paused = true;
  t.Restart(100);
  CHECK(t.paused);
  // Still frozen after restart — caller must un-pause separately.
  CHECK_FALSE(t.ShouldAdvance(1'000'000, 30'000));
}

TEST_CASE("Un-pausing does not by itself reset the deadline") {
  // Important for the old-firmware `/api/action/timer_restart` semantics
  // — the endpoint unpauses AND restarts, so callers that want
  // "resume without reset" have to do it in two steps. This test pins
  // that the primitive does not silently re-arm the clock on un-pause.
  RotationTimer t;
  t.last_change_ms = 0;
  t.paused = true;
  // Time moves forward while paused.
  CHECK_FALSE(t.ShouldAdvance(60'000, 30'000));
  t.paused = false;
  // On un-pause the already-elapsed 60s immediately satisfies the
  // 30s period — deadline was never frozen, only the decision was.
  CHECK(t.ShouldAdvance(60'000, 30'000));
}

TEST_CASE("Exact deadline boundary is inclusive") {
  RotationTimer t;
  t.last_change_ms = 0;
  CHECK(t.ShouldAdvance(30'000, 30'000));
}

TEST_CASE("Zero period advances on any tick > last_change_ms") {
  // Degenerate case — a 0 ms period means "advance every poll". Not
  // used in practice but pinning the behaviour prevents accidental
  // bug introductions if the period ever lands at 0.
  RotationTimer t;
  t.last_change_ms = 100;
  CHECK(t.ShouldAdvance(100, 0));
  CHECK(t.ShouldAdvance(101, 0));
}
