// Rotation-timer primitive — pure-logic core of ScreenManager's
// MaybeAutoRotate decision, pulled out so host tests can pin the
// pause + deadline-reset semantics without building the full
// ScreenManager (which depends on the EPD driver and fonts bundle).
//
// The struct tracks just two pieces of state: the "paused" flag and
// the last-change timestamp. ScreenManager reads/writes these in-line;
// the helpers below exist mostly for documentation + testability.

#pragma once

#include <cstdint>

namespace btclock {

struct RotationTimer {
  bool paused = false;
  int64_t last_change_ms = 0;

  // Answer "is it time to auto-advance?" given the current tick and
  // the configured period. Pause wins — returns false even if the
  // deadline would otherwise have passed. The caller is expected to
  // flip `last_change_ms = now_ms` when it accepts the advance, so
  // this helper is a read-only decision.
  constexpr bool ShouldAdvance(int64_t now_ms, int64_t period_ms) const {
    if (paused) return false;
    return now_ms - last_change_ms >= period_ms;
  }

  // Zero the deadline from `now_ms`. Pause state is preserved — so a
  // caller can restart while paused and the next un-pause still
  // enforces a full `period_ms` before the first auto-advance.
  constexpr void Restart(int64_t now_ms) { last_change_ms = now_ms; }
};

}  // namespace btclock
