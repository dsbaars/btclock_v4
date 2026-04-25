// Pure-logic decisions for the new-block event and the zap overlay.
//
// Small predicates, extracted here so host tests can pin the
// steal-focus + overlay-guard behaviour and the zap-overlay deadline
// arithmetic without pulling ScreenManager (which drags fonts + EPD +
// FreeRTOS into the test target).
//
// Wired into event_loop.cpp:
//   - when ConsumeNewBlock returns true the loop consults
//     ShouldSteal(pref, current_kind) before calling
//     ScreenManager::SetKind(kBlockHeight);
//   - when a zap lands the loop computes the overlay timeout via
//     ZapOverlayPolicy::ComputeTimeoutMs(timer_seconds) and passes it
//     through to ScreenManager::SetZapNotify.
// Keeping these rules here avoids spreading event policy between the
// event loop and the screen manager.

#pragma once

#include <cstdint>

#include "screens/screen_kind.hpp"

namespace btclock {

struct BlockEventPolicy {
  // Returns true when the main loop should jump the display to the
  // block-height screen in response to a new block. Rules:
  //   - steal_focus=false → never steal.
  //   - Already on kBlockHeight → no-op (the normal render path will
  //     refresh the digits).
  //   - kDebug + kNostrZap are respected: kDebug means the user is
  //     mid-debug and a yank would lose context; kNostrZap is a short-
  //     lived overlay with its own timeout that auto-restores the prior
  //     screen, so layering a steal underneath would skip the restore.
  //   - kCustom is NOT protected — a /api/show/text override is sticky
  //     until the next nav, but stealFocus is the user opting into
  //     "yes, please yank me off whatever I'm on when a block lands".
  //     A custom-text screen would otherwise stay up forever even
  //     though stealFocus is on.
  static constexpr bool ShouldSteal(bool steal_focus, ScreenType current) {
    if (!steal_focus) return false;
    switch (current) {
      case ScreenType::kBlockHeight:
      case ScreenType::kDebug:
      case ScreenType::kNostrZap:
        return false;
      default:
        return true;
    }
  }
};

struct ZapOverlayPolicy {
  // Documented fallback when timerSeconds is unset / zero. Matches
  // ScreenManager::kZapTimeoutMs — duplicated here so the pure-logic
  // helper stays free of the ScreenManager header. A compile-time
  // link check sits in the ScreenManager TU (see CheckZapTimeoutParity
  // below).
  static constexpr int64_t kFallbackMs = 8'000;

  // Translate the `timerSeconds` NVS pref into the overlay's visible-
  // time window. Non-positive input (unset pref, or the user saved 0
  // by accident) falls back to the documented default so the zap
  // doesn't vanish before the viewer reads it.
  static constexpr int64_t ComputeTimeoutMs(int64_t timer_seconds) {
    if (timer_seconds <= 0) return kFallbackMs;
    return timer_seconds * 1000;
  }
};

}  // namespace btclock
